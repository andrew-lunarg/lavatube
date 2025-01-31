#!/usr/bin/python2

import xml.etree.ElementTree as ET
import re

INSTANCE_CHAIN_PARAMETERS = ["VkInstance", "VkPhysicalDevice"]
DEVICE_CHAIN_PARAMETERS = ["VkDevice", "VkQueue", "VkCommandBuffer"]

tree = ET.parse('external/Vulkan-Headers/registry/vk.xml')
root = tree.getroot()
disabled = set()
disabled_functions = set()
functions = [] # must be ordered, so cannot use set
protected_funcs = {}
protected_types = {}
structures = []
disp_handles = []
nondisp_handles = []
all_handles = []
platforms = {}
extension_structs = set() # list of extension structs
type2sType = {} # map struct type -> sType enum
function_aliases = {} # goes from vendor extension -> core extension -> core
aliases_to_functions_map = {}
extension_tags = []
parents = {} # dictionary of lists
externally_synchronized = set() # tuples with (vulkan command, parameter name)

externally_synchronized_members = {
	'VkDescriptorSetAllocateInfo' : [ 'descriptorPool' ],
	'VkCommandBufferAllocateInfo' : [ 'commandPool' ],
	'VkSwapchainCreateInfoKHR' : [ 'surface', 'oldSwapchain' ],
	'VkDebugMarkerObjectTagInfoEXT' : [ 'object' ],
	'VkDebugMarkerObjectNameInfoEXT' : [ 'object' ],
	'VkDebugUtilsObjectNameInfoEXT' : [ 'objectHandle' ],
	'vkSetDebugUtilsObjectTagEXT' : [ 'objectHandle' ],
}

# We want to manually override some of these. We also add 'basetype' category types in here.
# This mapping is extended below with information from the XML.
type_mappings = {
	'char' : 'uint8_t',
	'int' : 'int32_t', # for all platforms we care about
	'long' : 'int64_t',
	'size_t' : 'uint64_t',
	'VkFlags' : 'uint32_t',
	'VkFlags64' : 'uint64_t',
	'void' : 'uint8_t', # buffer contents
	'xcb_visualid_t' : 'uint32_t',
}

# Parameters not named *Count with type uint32_t or size_t that need temporaries created for access to them by other parameters.
other_counts = {
	'VkPipelineShaderStageModuleIdentifierCreateInfoEXT' : [ 'identifierSize' ],
	'vkCreateRayTracingPipelinesKHR' : [ 'dataSize' ],
	'vkGetPipelineExecutableInternalRepresentationsKHR' : [ 'dataSize' ],
	'vkCreateGraphicsPipelines' : [ 'rasterizationSamples', 'dataSize', 'pRasterizationState' ],
	'vkCreateComputePipelines' : [ 'dataSize' ],
	'vkUpdateDescriptorSets' : [ 'dataSize', 'descriptorType' ],
	'vkCmdPushDescriptorSetKHR' : [ 'dataSize', 'descriptorType' ],
	'vkSetDebugUtilsObjectTagEXT' : [ 'tagSize' ],
	'vkCreateShaderModule' : [ 'codeSize' ],
	'vkCreateValidationCacheEXT' : [ 'initialDataSize' ],
	'vkDebugMarkerSetObjectTagEXT' : [ 'tagSize' ],
	'vkCreatePipelineCache' : [ 'initialDataSize' ],
}

# special call-me-twice query functions
special_count_funcs = {}

# dictionary values contain: name of variable holding created handle, name of variable holding number of handles to create (or hard-coded value), type of created handle
functions_create = {}

# Functions that destroy Vulkan objects
functions_destroy = {}

# Functions that can be called before an instance is created (loader implementations).
special_commands = []

# Functions that can be called before a logical device is created.
instance_chain_commands = []

# Functions that are called on existing logical devices, queues or command buffers.
device_chain_commands = []

feature_detection_structs = []
feature_detection_funcs = []

def str_contains_vendor(s):
	if not s: return False
	if 'KHR' in s or 'EXT' in s: return False # not a vendor and we want these
	if 'GOOGLE' in s or 'ARM' in s: return False # permit these
	for t in extension_tags:
		if s.endswith(t):
			return True
	return False

def init():
	detect_words = []
	with open('include/feature_detect.h', 'r') as f:
		for line in f:
			m = re.search('check_(\w+)', line)
			if m:
				detect_words.append(m.group(1))

	# Find extension tags
	for v in root.findall("tags/tag"):
			name = v.attrib.get('name')
			if name not in extension_tags: extension_tags.append(name)

	# Find all platforms
	for v in root.findall('platforms/platform'):
		name = v.attrib.get('name')
		prot = v.attrib.get('protect')
		platforms[name] = prot

	# Find ifdef conditionals (that we want to recreate) and disabled extensions (that we want to ignore)
	for v in root.findall('extensions/extension'):
		name = v.attrib.get('name')
		conditional = v.attrib.get('platform')
		supported = v.attrib.get('supported')
		if supported == 'disabled':
			for dc in v.findall('require/command'):
				disabled_functions.add(dc.attrib.get('name'))
			disabled.add(name)
		if conditional:
			for n in v.findall('require/command'):
				protected_funcs[n.attrib.get('name')] = platforms[conditional]
			for n in v.findall('require/type'):
				protected_types[n.attrib.get('name')] = platforms[conditional]

	# Find all structures and handles
	for v in root.findall('types/type'):
		category = v.attrib.get('category')
		name = v.attrib.get('name')
		if category == 'struct':
			sType = None
			for m in v.findall('member'):
				sType = m.attrib.get('values')
				if sType and 'VK_STRUCTURE_TYPE' in sType:
					break
			if sType:
				type2sType[name] = sType
			# Look for extensions
			extendstr = v.attrib.get('structextends')
			extends = []
			structures.append(name)
			if str_contains_vendor(sType): continue
			if name in detect_words:
				feature_detection_structs.append(name)
			if name in protected_types:
				continue # TBD: need a better way?
			if extendstr:
				assert sType, 'Failed to find structure type for %s' % name
				extends = extendstr.split(',')
			for e in extends:
				if str_contains_vendor(e): continue
				if str_contains_vendor(name): continue
				extension_structs.add(name)
		elif category == 'handle':
			if v.find('name') == None: # ignore aliases for now
				continue
			name = v.find('name').text
			parenttext = v.attrib.get('parent')
			if str_contains_vendor(name): continue
			if parenttext:
				parentsplit = parenttext.split(',')
				for p in parentsplit:
					if not name in parents:
						parents[name] = []
					parents[name].append(p)
			if v.find('type').text == 'VK_DEFINE_HANDLE':
				disp_handles.append(name)
			else: # non-dispatchable
				nondisp_handles.append(name)
			all_handles.append(name)
		elif category == 'enum':
			type_mappings[name] = 'uint32_t'
		elif category == 'basetype':
			name = v.find('name').text
			atype = v.find('type')
			if atype is not None: type_mappings[name] = atype.text
		elif category == 'bitmask':
			# ignore aliases for now
			if v.find('name') == None:
				continue
			atype = v.find('type').text
			name = v.find('name').text
			if atype == 'VkFlags64':
				type_mappings[name] = 'uint64_t'
			elif atype == 'VkFlags':
				type_mappings[name] = 'uint32_t'
			else:
				assert false, 'Unknown bitmask type: %s' % atype

	# Find aliases
	for v in root.findall("commands/command"):
		if v.attrib.get('alias'):
			alias_name = v.attrib.get('alias')
			name = v.attrib.get('name')
			if str_contains_vendor(name): continue
			function_aliases[name] = alias_name
			if not alias_name in aliases_to_functions_map:
				aliases_to_functions_map[alias_name] = []
			aliases_to_functions_map[alias_name].append(name)
		else:
			proto = v.find('proto')
			name = proto.find('name').text

	# Find all commands/functions
	for v in root.findall("commands/command"):
		alias_name = v.attrib.get('alias')
		targets = []
		if not alias_name:
			targets.append(v.findall('proto/name')[0].text)
		else:
			continue # generate from aliased target instead

		origname = v.findall('proto/name')[0].text
		for a in aliases_to_functions_map.get(origname, []): # generate for any aliases as well, since they do not have proper data themselves
			targets.append(a)

		for name in targets:
			is_instance_chain_command = False
			is_device_chain_command = False

			if 'vkGet' in name or 'vkEnum' in name: # find special call-me-twice functions
				typename = None
				lastname = None
				counttype = None
				params = v.findall('param')
				assert len(params) > 0
				for param in params:
					ptype = param.text
					ptype = param.find('type').text
					pname = param.find('name').text
					optional = param.attrib.get('optional')
					typename = ptype # always the last parameter
					lastname = pname
					if not optional: continue
					if not 'true' in optional or not 'false' in optional: continue
					if pname and ptype and ('Count' in pname or 'Size' in pname) and (ptype == 'uint32_t' or ptype == 'size_t'):
						countname = pname
						counttype = ptype
				if counttype != None:
					if typename == 'void': typename = 'char'
					if 'vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCounters' in name: # special case it for now, only case of double out param
						special_count_funcs[name] = [ countname, counttype, [ [ 'pCounters', 'VkPerformanceCounterKHR' ], [ lastname, typename ] ] ]
					else:
						special_count_funcs[name] = [ countname, counttype, [ [ lastname, typename ] ] ]

			if 'vkFree' in name or 'vkDestroy' in name or 'vkCreate' in name or 'vkAllocate' in name:
				countname = "1"
				typename = None
				lastname = None
				params = v.findall('param')
				assert len(params) > 0
				for param in params:
					ptype = param.text
					ptype = param.find('type').text
					pname = param.find('name').text
					if pname and ptype and ('Count' in pname or 'Size' in pname) and ptype == 'uint32_t':
						countname = pname
					elif name == 'vkAllocateDescriptorSets': # some ugly exceptions where the count is inside a passed in struct
						countname = 'descriptorSetCount'
					elif name == 'vkAllocateCommandBuffers':
						countname = 'commandBufferCount'
					if pname == 'pAllocator' or pname == None:
						continue
					typename = ptype # always the last parameter
					lastname = pname
				if 'vkFree' in name or 'vkDestroy' in name:
					functions_destroy[name] = [ lastname, countname, typename ]
				elif 'vkCreate' in name or 'vkAllocate' in name:
					functions_create[name] = [ lastname, countname, typename ]

			# Find externally synchronized parameters
			params = v.findall('param')
			for param in params:
				extern = param.attrib.get('externsync')
				if not extern: continue # not externally synchronized
				typename = param.find('name').text
				type = param.find('type').text
				if extern == 'true':
					externally_synchronized.add((name, typename))
					continue
				all_exts = extern.split(',')
				for ext in all_exts:
					if '>' in ext:
						ext = ext.replace('[]', '')
						externally_synchronized.add((type, ext.split('>')[1]))
					elif '[' in ext:
						externally_synchronized.add((type, ext.split('.')[1]))
					else: assert False, '%s not parsed correctly' % ext

			params = v.findall('param/type')
			for param in params:
				ptype = param.text
				if ptype in INSTANCE_CHAIN_PARAMETERS:
					is_instance_chain_command = True
					break
				elif ptype in DEVICE_CHAIN_PARAMETERS:
					is_device_chain_command = True
					break

			if name not in functions:
				functions.append(name)
				if name in detect_words:
					feature_detection_funcs.append(name)

			if is_instance_chain_command:
				if name not in instance_chain_commands: instance_chain_commands.append(name)
			elif is_device_chain_command:
				if name not in device_chain_commands: device_chain_commands.append(name)
			else:
				if name not in special_commands: special_commands.append(name)
