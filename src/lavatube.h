// Vulkan trace common code

#pragma once

#define LAVATUBE_VERSION_MAJOR 0
#define LAVATUBE_VERSION_MINOR 0
#define LAVATUBE_VERSION_PATCH 1

#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"
#include "vk_wrapper_auto.h"
#include "util.h"
#include "rangetracking.h"
#include "vulkan_ext.h"
#include "containers.h"

#include <unordered_set>
#include <list>

class lava_file_reader;
class lava_file_writer;

using lava_trace_func = PFN_vkVoidFunction;

/// pending memory checking, wait on fence
enum QueueState
{
	QUEUE_STATE_NONE = 0,
	QUEUE_STATE_PENDING_EVENTS = 2,
};

enum
{
	PACKET_API_CALL = 2,
	PACKET_THREAD_BARRIER = 3,
	PACKET_IMAGE_UPDATE = 4,
	PACKET_BUFFER_UPDATE = 5,
};

struct trackable
{
	uint32_t index = 0;
	int frame_created = 0;
	int frame_destroyed = -1;
	std::string name;
	trackable(int _created) : frame_created(_created) {}
	trackable() {}
	int8_t tid = -1; // object last modified in this thread
	uint16_t call = 0; // object last modified at this thread local call number

	void self_test() const
	{
		assert(frame_destroyed == -1 || frame_destroyed >= 0);
		assert(frame_created >= 0);
		assert(tid != -1);
	}
};

// TBD is this entire block only useful to tracer now?
struct trackedmemory : trackable
{
	using trackable::trackable; // inherit constructor

	// the members below are ephemeral and not saved to disk:

	VkMemoryPropertyFlags propertyFlags = 0;

	/// Current mappping offset
	VkDeviceSize offset = 0;
	/// Current mapping size
	VkDeviceSize size = 0;

	/// Total size
	VkDeviceSize allocationSize = 0;

	/// Sparse copy of entire memory object. Compare against it when diffing
	/// using the touched ranges below. We only do this for memory objects that
	/// are mapped at least once.
	char* clone = nullptr;

	/// Original memory area
	char* ptr = nullptr;

	/// Tracking all memory exposed to client through memory mapping.
	exposure exposed;

	VkDeviceMemory backing = VK_NULL_HANDLE;

	void self_test() const
	{
		assert(backing != VK_NULL_HANDLE);
		assert(offset + size <= allocationSize);
		assert(exposed.span().last <= allocationSize);
		assert(allocationSize != VK_WHOLE_SIZE);
		trackable::self_test();
	}
};

struct trackeddevice : trackable
{
	using trackable::trackable; // inherit constructor
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
};

struct trackedobject : trackable
{
	using trackable::trackable; // inherit constructor
	VkDeviceMemory backing = (VkDeviceMemory)0;
	VkDeviceSize size = 0;
	VkDeviceSize offset = 0; // our offset into our backing memory
	VkMemoryRequirements req = {};
	VkObjectType type = VK_OBJECT_TYPE_UNKNOWN;
	uint64_t written = 0; // bytes written out for this object
	uint32_t updates = 0; // number of times it was updated
	bool accessible = false; // whether our backing memory is host visible and understandable

	void self_test() const
	{
		assert(type != VK_OBJECT_TYPE_UNKNOWN);
		assert(size != VK_WHOLE_SIZE);
		trackable::self_test();
	}
};

struct trackedbuffer : trackedobject
{
	using trackedobject::trackedobject; // inherit constructor
	VkBufferCreateFlags flags = VK_BUFFER_CREATE_FLAG_BITS_MAX_ENUM;
	VkSharingMode sharingMode = VK_SHARING_MODE_MAX_ENUM;
	VkBufferUsageFlags usage = VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM;

	void self_test() const
	{
		assert(flags != VK_BUFFER_CREATE_FLAG_BITS_MAX_ENUM);
		assert(sharingMode != VK_SHARING_MODE_MAX_ENUM);
		assert(usage != VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM);
		trackedobject::self_test();
	}
};

struct trackedimage : trackedobject
{
	using trackedobject::trackedobject; // inherit constructor
	VkImageTiling tiling = VK_IMAGE_TILING_MAX_ENUM;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM;
	VkSharingMode sharingMode = VK_SHARING_MODE_MAX_ENUM;
	VkImageType imageType = VK_IMAGE_TYPE_MAX_ENUM;
	VkImageCreateFlags flags = VK_IMAGE_CREATE_FLAG_BITS_MAX_ENUM;
	VkFormat format = VK_FORMAT_MAX_ENUM;
	bool is_swapchain_image = false;

	void self_test() const
	{
		assert(tiling != VK_IMAGE_TILING_MAX_ENUM);
		assert(usage != VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM);
		assert(sharingMode != VK_SHARING_MODE_MAX_ENUM);
		assert(imageType != VK_IMAGE_TYPE_MAX_ENUM);
		assert(flags != VK_IMAGE_CREATE_FLAG_BITS_MAX_ENUM);
		assert(format != VK_FORMAT_MAX_ENUM);
		trackedobject::self_test();
	}
};

struct trackedimageview : trackable
{
	using trackable::trackable; // inherit constructor
	VkImage image = VK_NULL_HANDLE;
	uint32_t image_index = CONTAINER_INVALID_INDEX;
	VkImageSubresourceRange subresourceRange = {};
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageViewType viewType = (VkImageViewType)0;
	VkComponentMapping components = {};
	VkImageViewCreateFlags flags = (VkImageViewCreateFlags)0;

	void self_test() const
	{
		assert(image != VK_NULL_HANDLE);
		assert(image_index != CONTAINER_INVALID_INDEX);
		assert(format != VK_FORMAT_UNDEFINED);
		trackable::self_test();
	}
};

struct trackedbufferview : trackable
{
	using trackable::trackable; // inherit constructor
	VkBuffer buffer = VK_NULL_HANDLE;
	uint32_t buffer_index = CONTAINER_INVALID_INDEX;
	VkDeviceSize offset = 0;
	VkDeviceSize range = VK_WHOLE_SIZE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkBufferViewCreateFlags flags = (VkBufferViewCreateFlags)0;

	void self_test() const
	{
		assert(buffer != VK_NULL_HANDLE);
		assert(buffer_index != CONTAINER_INVALID_INDEX);
		assert(format != VK_FORMAT_UNDEFINED);
		trackable::self_test();
	}
};

struct trackedswapchain : trackable
{
	using trackable::trackable; // inherit constructor
	VkSwapchainCreateInfoKHR info = {};

	void self_test() const
	{
		assert(info.sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
		trackable::self_test();
	}
};

struct trackedswapchain_trace : trackedswapchain
{
	using trackedswapchain::trackedswapchain; // inherit constructor
	VkQueue queue = VK_NULL_HANDLE;

	void self_test() const
	{
		assert(queue != VK_NULL_HANDLE);
		trackedswapchain::self_test();
	}
};

struct trackedswapchain_replay : trackedswapchain
{
	using trackedswapchain::trackedswapchain; // inherit constructor
	std::vector<VkImage> pSwapchainImages;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	uint32_t next_swapchain_image = 0;
	uint32_t next_stored_image = 0;
	std::vector<VkImage> virtual_images;
	std::vector<VkCommandBuffer> virtual_cmdbuffers;
	VkCommandPool virtual_cmdpool = VK_NULL_HANDLE;
	VkSemaphore virtual_semaphore = VK_NULL_HANDLE;
	VkImageCopy virtual_image_copy_region = {};
	std::vector<VkFence> virtual_fences;
	std::vector<bool> inflight; // is this entry in use already
	bool initialized = false;
	VkDevice device = VK_NULL_HANDLE;

	void self_test() const
	{
		if (!initialized) return;
		assert(swapchain != VK_NULL_HANDLE);
		if (p__virtualswap)
		{
			assert(virtual_cmdpool != VK_NULL_HANDLE);
			assert(virtual_semaphore != VK_NULL_HANDLE);
			for (const auto& v : virtual_cmdbuffers) { assert(v != VK_NULL_HANDLE); (void)v; }
			for (const auto& v : virtual_images) { assert(v != VK_NULL_HANDLE); (void)v; }
			for (const auto& v : virtual_fences) { assert(v != VK_NULL_HANDLE); (void)v; }
		}
		trackedswapchain::self_test();
	}
};

struct trackedfence : trackable
{
	using trackable::trackable; // inherit constructor
	VkFenceCreateFlags flags = (VkFenceCreateFlags)0;

	// tracer only
	int frame_delay = -1; // delay fuse
};

struct trackedpipeline : trackable
{
	using trackable::trackable; // inherit constructor
	VkPipelineBindPoint type = VK_PIPELINE_BIND_POINT_MAX_ENUM;
	VkPipelineCreateFlags flags = 0;
	VkPipelineCache cache = VK_NULL_HANDLE;

	void self_test() const
	{
		assert(type != VK_PIPELINE_BIND_POINT_MAX_ENUM);
		trackable::self_test();
	}
};

struct trackedcmdbuffer_trace : trackable
{
	using trackable::trackable; // inherit constructor
	VkCommandPool pool = VK_NULL_HANDLE;
	uint32_t pool_index = CONTAINER_INVALID_INDEX;
	VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_MAX_ENUM;
	std::unordered_map<trackedobject*, exposure> touched; // track memory updates

	void touch(trackedobject* data, VkDeviceSize offset, VkDeviceSize size)
	{
		if (!data->accessible) return;
		if (size == VK_WHOLE_SIZE) size = data->size - offset;
		touched[data].add_os(offset, size);
	}

	void touch_merge(const std::unordered_map<trackedobject*, exposure>& other)
	{
		for (const auto& pair : other)
		{
			if (touched.count(pair.first) > 0) for (auto& r : pair.second.list()) touched[pair.first].add(r.first, r.last); // merge in
			else touched[pair.first] = pair.second; // just insert
		}
	}

	void self_test() const
	{
		for (const auto& pair : touched) { assert(pair.first->accessible); pair.first->self_test(); pair.second.self_test(); }
		assert(pool != VK_NULL_HANDLE);
		assert(pool_index != CONTAINER_INVALID_INDEX);
		assert(level != VK_COMMAND_BUFFER_LEVEL_MAX_ENUM);
		trackable::self_test();
	}
};

struct trackeddescriptorset_trace : trackable
{
	using trackable::trackable; // inherit constructor
	VkDescriptorPool pool = VK_NULL_HANDLE;
	uint32_t pool_index = CONTAINER_INVALID_INDEX;
	std::unordered_map<trackedobject*, exposure> touched; // track memory updates

	void touch(trackedobject* data, VkDeviceSize offset, VkDeviceSize size)
	{
		if (!data->accessible) return;
		if (size == VK_WHOLE_SIZE) size = data->size - offset;
		touched[data].add_os(offset, size);
	}

	void self_test() const
	{
		assert(pool != VK_NULL_HANDLE);
		assert(pool_index != CONTAINER_INVALID_INDEX);
		for (const auto& pair : touched) { pair.first->self_test(); pair.second.self_test(); }
		trackable::self_test();
	}
};

struct trackedqueue_trace : trackable
{
	using trackable::trackable; // inherit constructor
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queueIndex = 0;
	uint32_t queueFamily = 0;
	VkQueueFlags queueFlags = VK_QUEUE_FLAG_BITS_MAX_ENUM;

	void self_test() const
	{
		assert(device != VK_NULL_HANDLE);
		assert(queueFlags != VK_QUEUE_FLAG_BITS_MAX_ENUM);
		trackable::self_test();
	}
};

struct trackedevent_trace : trackable
{
	using trackable::trackable; // inherit constructor
};

struct trackeddescriptorpool_trace : trackable
{
	using trackable::trackable; // inherit constructor
};

struct trackedcommandpool_trace : trackable
{
	using trackable::trackable; // inherit constructor
	std::unordered_set<trackedcmdbuffer_trace*> commandbuffers;
};

struct trackedrenderpass : trackable
{
	using trackable::trackable; // inherit constructor
	std::vector<VkAttachmentDescription> attachments;
};

struct trackedframebuffer : trackable
{
	using trackable::trackable; // inherit constructor
	std::vector<trackedimageview*> imageviews;
	VkFramebufferCreateFlags flags = (VkFramebufferCreateFlags)0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t layers = 0;

	void self_test() const
	{
		for (const auto v : imageviews) v->self_test();
		trackable::self_test();
	}
};
