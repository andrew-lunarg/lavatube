// Preload test

#include "tests/common.h"
#include <inttypes.h>
#include "util_auto.h"

#define TEST_NAME "tracing_6"
#define NUM_BUFFERS 3
#define NUM_THREADS 10
#define NUM_TIMES 2
#define PACKET_SIZE (1000)
#define BUF_SIZE (PACKET_SIZE + 99) // just bigger than our packet size

static vulkan_setup_t vulkan;

static void thread_runner(int tid)
{
	VkResult result;

	// Store some data for N frames this thread
	for (int i = 0; i < NUM_TIMES; i++)
	{
		VkBuffer buffer[NUM_BUFFERS];
		VkBufferCreateInfo bufferCreateInfo = {};
		bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCreateInfo.size = BUF_SIZE;
		bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		for (unsigned j = 0; j < NUM_BUFFERS; j++)
		{
			result = trace_vkCreateBuffer(vulkan.device, &bufferCreateInfo, nullptr, &buffer[j]);
			check(result);
		}
		VkMemoryRequirements req;
		trace_vkGetBufferMemoryRequirements(vulkan.device, buffer[0], &req);
		uint32_t memoryTypeIndex = get_device_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VkMemoryAllocateInfo pAllocateMemInfo = {};
		pAllocateMemInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		pAllocateMemInfo.memoryTypeIndex = memoryTypeIndex;
		pAllocateMemInfo.allocationSize = req.size * NUM_BUFFERS;
		VkDeviceMemory memory = 0;
		result = trace_vkAllocateMemory(vulkan.device, &pAllocateMemInfo, nullptr, &memory);
		check(result);
		assert(memory != 0);
		char* ptr = nullptr;
		result = trace_vkMapMemory(vulkan.device, memory, 0, pAllocateMemInfo.allocationSize, 0, (void**)&ptr);
		memset(ptr, tid, pAllocateMemInfo.allocationSize);
		VkMappedMemoryRange flush = {};
		flush.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		flush.memory = memory;
		flush.offset = 0;
		flush.size = pAllocateMemInfo.allocationSize;
		trace_vkFlushMappedMemoryRanges(vulkan.device, 1, &flush);
		trace_vkUnmapMemory(vulkan.device, memory);
		uint64_t offset = 0;
		for (unsigned j = 0; j < NUM_BUFFERS; j++)
		{
			trace_vkBindBufferMemory(vulkan.device, buffer[j], memory, offset);
			offset += req.size;
		}
		trace_vkSyncBufferTRACETOOLTEST(vulkan.device, buffer[i]);
		trace_vkAssertBufferTRACETOOLTEST(vulkan.device, buffer[i]);
		for (unsigned j = 0; j < NUM_BUFFERS; j++)
		{
			trace_vkDestroyBuffer(vulkan.device, buffer[j], nullptr);
		}
		trace_vkFreeMemory(vulkan.device, memory, nullptr);
		trace_vkFrameEndTRACETOOLTEST(vulkan.device);
	}
}

static void trace_me()
{
	vulkan = test_init(TEST_NAME, PACKET_SIZE); // override the default chunk size

	// Check that you can access them from vkGetDeviceProcAddr, although we won't
	auto ptr = trace_vkGetDeviceProcAddr(vulkan.device, "vkAssertBufferTRACETOOLTEST");
	assert(ptr != nullptr);
	ptr = trace_vkGetDeviceProcAddr(vulkan.device, "vkSyncBufferTRACETOOLTEST");
	assert(ptr != nullptr);
	ptr = trace_vkGetDeviceProcAddr(vulkan.device, "vkFrameEndTRACETOOLTEST");
	assert(ptr != nullptr);

	for (int i = 0; i < NUM_THREADS; i++)
	{
		std::thread* t = new std::thread(thread_runner, i);
		t->join();
		delete t;
	}

	test_done(vulkan);
}

static bool getnext(lava_file_reader& t)
{
	const uint8_t instrtype = t.step();
	if (instrtype == PACKET_API_CALL)
	{
		const uint16_t apicall = t.read_uint16_t();
		(void)t.read_int32_t();
		DLOG("[t%02d %06d] %s", t.thread_index(), (int)t.parent->thread_call_numbers->at(t.thread_index()).load(std::memory_order_relaxed) + 1, get_function_name(apicall));
		lava_replay_func api = retrace_getcall(apicall);
		api(t);
		if (apicall == VKFRAMEENDTRACETOOLTEST) ILOG("Global frame %d (local %d in thread %d) from vkFrameEndTRACETOOLTEST", t.parent->global_frame.load(), t.local_frame, t.thread_index());
		t.parent->thread_call_numbers->at(t.thread_index()).fetch_add(1, std::memory_order_relaxed);
		suballoc_internal_test();
		t.pool.reset();
	}
	else if (instrtype == PACKET_THREAD_BARRIER)
	{
		t.read_barrier();
	}
	else if (instrtype == PACKET_BUFFER_UPDATE)
	{
		const uint32_t device_index = t.read_handle();
		const uint32_t buffer_index = t.read_handle();
		buffer_update(t, device_index, buffer_index);
	}
	else if (instrtype != 0) ABORT("Unexpected packet type %d in thread %d", (int)instrtype, (int)t.thread_index());
	suballoc_internal_test();
	return (instrtype != 0);
}

static void retrace_me(lava_reader* r, int tid)
{
	lava_file_reader& t = r->file_reader(tid);
	suballoc_internal_test();
	while (getnext(t)) {}
	suballoc_internal_test();
}

static void read_test(int start, int end, bool preload)
{
	lava_reader reader(TEST_NAME ".vk");
	reader.parameters(start, end, preload);
	std::vector<std::thread*> threads; // main thread + helper threads
	for (int tid = 0; tid < NUM_THREADS + 1; tid++)
	{
		std::thread* t = new std::thread(retrace_me, &reader, tid);
		threads.push_back(t);
	}
	for (std::thread* t : threads)
	{
		t->join();
		delete t;
	}
	threads.clear();
	int remaining = suballoc_internal_test();
	assert(remaining == 0); // everything should be destroyed now
}

int main()
{
	trace_me();
	read_test(0, -1, false);
	return 0;
}
