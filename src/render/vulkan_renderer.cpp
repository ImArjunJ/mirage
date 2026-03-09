#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/log.hpp"
#include "media/media.hpp"
#include "platform/window.hpp"
#include "render/render.hpp"

namespace mirage::render {

namespace {

constexpr uint32_t max_frames_in_flight = 2;

VkShaderModule load_shader_module(VkDevice device, const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        mirage::log::error("Failed to open shader file: {}", path);
        return VK_NULL_HANDLE;
    }
    auto file_size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(file_size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(file_size));
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = file_size;
    create_info.pCode = reinterpret_cast<const uint32_t*>(buffer.data());
    VkShaderModule shader_module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &create_info, nullptr, &shader_module);
    return shader_module;
}

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter,
                          VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    mirage::log::error("Failed to find suitable memory type");
    return 0;
}

struct vk_buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

vk_buffer create_buffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                        VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    vk_buffer result{};
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer);
    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device, result.buffer, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex =
        find_memory_type(physical_device, mem_reqs.memoryTypeBits, properties);
    vkAllocateMemory(device, &alloc_info, nullptr, &result.memory);
    vkBindBufferMemory(device, result.buffer, result.memory, 0);
    return result;
}

struct vk_image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

vk_image create_image(VkDevice device, VkPhysicalDevice physical_device, uint32_t w, uint32_t h,
                      VkFormat format, VkImageUsageFlags usage, VkMemoryPropertyFlags properties) {
    vk_image result{};
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = w;
    image_info.extent.height = h;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    vkCreateImage(device, &image_info, nullptr, &result.image);
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, result.image, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex =
        find_memory_type(physical_device, mem_reqs.memoryTypeBits, properties);
    vkAllocateMemory(device, &alloc_info, nullptr, &result.memory);
    vkBindImageMemory(device, result.image, result.memory, 0);
    return result;
}

void transition_image_layout(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void copy_buffer_to_image(VkCommandBuffer cmd, VkBuffer buffer, VkImage image, uint32_t w,
                          uint32_t h) {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

VkCommandBuffer begin_one_shot(VkDevice device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = pool;
    alloc_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &alloc_info, &cmd);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);
    return cmd;
}

void end_one_shot(VkDevice device, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

void upload_texture(VkDevice device, VkPhysicalDevice physical_device, VkCommandPool pool,
                    VkQueue queue, VkImage image, const std::byte* data, uint32_t stride,
                    uint32_t w, uint32_t h, uint32_t pixel_size) {
    VkDeviceSize buffer_size = static_cast<VkDeviceSize>(w) * h * pixel_size;
    auto staging =
        create_buffer(device, physical_device, buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    vkMapMemory(device, staging.memory, 0, buffer_size, 0, &mapped);
    auto* dst = static_cast<uint8_t*>(mapped);
    auto src = reinterpret_cast<const uint8_t*>(data);
    for (uint32_t row = 0; row < h; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * w * pixel_size,
                    src + static_cast<size_t>(row) * stride, static_cast<size_t>(w) * pixel_size);
    }
    vkUnmapMemory(device, staging.memory);

    auto cmd = begin_one_shot(device, pool);
    transition_image_layout(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_buffer_to_image(cmd, staging.buffer, image, w, h);
    transition_image_layout(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_one_shot(device, pool, queue, cmd);

    vkDestroyBuffer(device, staging.buffer, nullptr);
    vkFreeMemory(device, staging.memory, nullptr);
}

VkImageView create_image_view(VkDevice device, VkImage image, VkFormat format) {
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(device, &view_info, nullptr, &view);
    return view;
}

VkSampler create_sampler(VkDevice device) {
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0F;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(device, &sampler_info, nullptr, &sampler);
    return sampler;
}

}  // namespace

std::unique_ptr<render_window> render_window::create(std::string_view title, uint32_t width,
                                                     uint32_t height) {
    auto win = std::unique_ptr<render_window>(new render_window());
    win->thread_ =
        std::thread(&render_window::thread_main, win.get(), std::string(title), width, height);
    while (!win->started_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return win;
}

void render_window::submit_frame(media::decoded_frame frame) {
    std::lock_guard lock(frame_mutex_);
    pending_frame_ = std::move(frame);
}

render_window::~render_window() {
    open_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void render_window::thread_main(const std::string& title, uint32_t width, uint32_t height) {
    auto win = platform::window::create(title, width, height);
    if (!win) {
        mirage::log::error("Failed to create window");
        started_.store(true, std::memory_order_release);
        return;
    }

    auto extensions = win->required_vulkan_extensions();

    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> available_layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());

    bool validation_available = false;
    for (const auto& layer : available_layers) {
        if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation") {
            validation_available = true;
            break;
        }
    }

    std::vector<const char*> layers;
    if (validation_available) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "mirage";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "mirage";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    instance_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        mirage::log::error("Failed to create Vulkan instance");
        started_.store(true, std::memory_order_release);
        return;
    }

    VkSurfaceKHR surface = win->create_vulkan_surface(instance);
    if (surface == VK_NULL_HANDLE) {
        mirage::log::error("Failed to create Vulkan surface");
        vkDestroyInstance(instance, nullptr);
        started_.store(true, std::memory_order_release);
        return;
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count == 0) {
        mirage::log::error("No Vulkan physical devices found");
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        started_.store(true, std::memory_order_release);
        return;
    }

    std::vector<VkPhysicalDevice> physical_devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data());

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t graphics_family = 0;

    auto find_queue_family = [&](VkPhysicalDevice dev) -> std::optional<uint32_t> {
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_family_count, queue_families.data());
        for (uint32_t i = 0; i < queue_family_count; ++i) {
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present_support);
            if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
                return i;
            }
        }
        return std::nullopt;
    };

    for (auto dev : physical_devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        auto family = find_queue_family(dev);
        if (family && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical_device = dev;
            graphics_family = *family;
            mirage::log::info("Selected discrete GPU: {}", props.deviceName);
            break;
        }
    }

    if (physical_device == VK_NULL_HANDLE) {
        for (auto dev : physical_devices) {
            auto family = find_queue_family(dev);
            if (family) {
                physical_device = dev;
                graphics_family = *family;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(dev, &props);
                mirage::log::info("Selected GPU: {}", props.deviceName);
                break;
            }
        }
    }

    if (physical_device == VK_NULL_HANDLE) {
        mirage::log::error("No suitable Vulkan device found");
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        started_.store(true, std::memory_order_release);
        return;
    }

    float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = graphics_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS) {
        mirage::log::error("Failed to create Vulkan device");
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        started_.store(true, std::memory_order_release);
        return;
    }

    VkQueue graphics_queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);

    auto choose_surface_format = [&]() {
        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count,
                                             formats.data());
        for (const auto& fmt : formats) {
            if (fmt.format == VK_FORMAT_B8G8R8A8_UNORM &&
                fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return fmt;
            }
        }
        return formats[0];
    };

    auto surface_format = choose_surface_format();

    auto create_swapchain_resources = [&](VkSwapchainKHR old_swapchain, VkSwapchainKHR& swapchain,
                                          std::vector<VkImage>& swapchain_images,
                                          std::vector<VkImageView>& swapchain_image_views,
                                          VkExtent2D& swapchain_extent) {
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities);

        if (capabilities.currentExtent.width != UINT32_MAX) {
            swapchain_extent = capabilities.currentExtent;
        } else {
            auto [w, h] = win->size_pixels();
            swapchain_extent.width =
                std::clamp(w, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            swapchain_extent.height = std::clamp(h, capabilities.minImageExtent.height,
                                                 capabilities.maxImageExtent.height);
        }

        uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
            image_count = capabilities.maxImageCount;
        }

        uint32_t present_mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count,
                                                  nullptr);
        std::vector<VkPresentModeKHR> present_modes(present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count,
                                                  present_modes.data());
        auto chosen_present_mode = VK_PRESENT_MODE_FIFO_KHR;
        for (auto mode : present_modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                chosen_present_mode = mode;
                break;
            }
        }
        VkSwapchainCreateInfoKHR sc_info{};
        sc_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sc_info.surface = surface;
        sc_info.minImageCount = image_count;
        sc_info.imageFormat = surface_format.format;
        sc_info.imageColorSpace = surface_format.colorSpace;
        sc_info.imageExtent = swapchain_extent;
        sc_info.imageArrayLayers = 1;
        sc_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sc_info.preTransform = capabilities.currentTransform;
        sc_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sc_info.presentMode = chosen_present_mode;
        sc_info.clipped = VK_TRUE;
        sc_info.oldSwapchain = old_swapchain;

        vkCreateSwapchainKHR(device, &sc_info, nullptr, &swapchain);

        uint32_t sc_image_count = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &sc_image_count, nullptr);
        swapchain_images.resize(sc_image_count);
        vkGetSwapchainImagesKHR(device, swapchain, &sc_image_count, swapchain_images.data());

        swapchain_image_views.resize(sc_image_count);
        for (uint32_t i = 0; i < sc_image_count; ++i) {
            swapchain_image_views[i] =
                create_image_view(device, swapchain_images[i], surface_format.format);
        }
    };

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_image_views;
    VkExtent2D swapchain_extent{};

    create_swapchain_resources(VK_NULL_HANDLE, swapchain, swapchain_images, swapchain_image_views,
                               swapchain_extent);

    VkAttachmentDescription color_attachment{};
    color_attachment.format = surface_format.format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass);

    auto create_framebuffers = [&](const std::vector<VkImageView>& views, VkExtent2D extent,
                                   std::vector<VkFramebuffer>& framebuffers) {
        framebuffers.resize(views.size());
        for (size_t i = 0; i < views.size(); ++i) {
            VkFramebufferCreateInfo fb_info{};
            fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_info.renderPass = render_pass;
            fb_info.attachmentCount = 1;
            fb_info.pAttachments = &views[i];
            fb_info.width = extent.width;
            fb_info.height = extent.height;
            fb_info.layers = 1;
            vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffers[i]);
        }
    };

    std::vector<VkFramebuffer> framebuffers;
    create_framebuffers(swapchain_image_views, swapchain_extent, framebuffers);

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ds_layout_info{};
    ds_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds_layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
    ds_layout_info.pBindings = bindings.data();

    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(device, &ds_layout_info, nullptr, &descriptor_set_layout);

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;

    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout);

    auto vert_module = load_shader_module(device, MIRAGE_SHADER_DIR "/nv12_to_rgb.vert.spv");
    auto frag_module = load_shader_module(device, MIRAGE_SHADER_DIR "/nv12_to_rgb.frag.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages{};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vert_module;
    shader_stages[0].pName = "main";
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = frag_module;
    shader_stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0F;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend_attachment;

    std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT,
                                                    VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = static_cast<uint32_t>(shader_stages.size());
    pipeline_info.pStages = shader_stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    VkPipeline graphics_pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                              &graphics_pipeline);

    vkDestroyShaderModule(device, vert_module, nullptr);
    vkDestroyShaderModule(device, frag_module, nullptr);

    std::array<VkDescriptorPoolSize, 1> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 2;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    pool_info.maxSets = 1;

    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool);

    VkDescriptorSetAllocateInfo ds_alloc_info{};
    ds_alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc_info.descriptorPool = descriptor_pool;
    ds_alloc_info.descriptorSetCount = 1;
    ds_alloc_info.pSetLayouts = &descriptor_set_layout;

    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device, &ds_alloc_info, &descriptor_set);

    VkCommandPoolCreateInfo cmd_pool_info{};
    cmd_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmd_pool_info.queueFamilyIndex = graphics_family;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &cmd_pool_info, nullptr, &command_pool);

    std::array<VkCommandBuffer, max_frames_in_flight> command_buffers{};
    VkCommandBufferAllocateInfo cmd_alloc_info{};
    cmd_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc_info.commandPool = command_pool;
    cmd_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc_info.commandBufferCount = max_frames_in_flight;
    vkAllocateCommandBuffers(device, &cmd_alloc_info, command_buffers.data());

    std::array<VkSemaphore, max_frames_in_flight> image_available_semaphores{};
    std::array<VkSemaphore, max_frames_in_flight> render_finished_semaphores{};
    std::array<VkFence, max_frames_in_flight> in_flight_fences{};

    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < max_frames_in_flight; ++i) {
        vkCreateSemaphore(device, &sem_info, nullptr, &image_available_semaphores[i]);
        vkCreateSemaphore(device, &sem_info, nullptr, &render_finished_semaphores[i]);
        vkCreateFence(device, &fence_info, nullptr, &in_flight_fences[i]);
    }

    vk_image y_image{};
    vk_image uv_image{};
    VkImageView y_view = VK_NULL_HANDLE;
    VkImageView uv_view = VK_NULL_HANDLE;
    VkSampler y_sampler = VK_NULL_HANDLE;
    VkSampler uv_sampler = VK_NULL_HANDLE;
    uint32_t tex_w = 0;
    uint32_t tex_h = 0;
    bool has_texture = false;

    auto destroy_nv12_textures = [&]() {
        if (!has_texture) {
            return;
        }
        vkDestroyImageView(device, y_view, nullptr);
        vkDestroyImageView(device, uv_view, nullptr);
        vkDestroySampler(device, y_sampler, nullptr);
        vkDestroySampler(device, uv_sampler, nullptr);
        vkDestroyImage(device, y_image.image, nullptr);
        vkFreeMemory(device, y_image.memory, nullptr);
        vkDestroyImage(device, uv_image.image, nullptr);
        vkFreeMemory(device, uv_image.memory, nullptr);
        has_texture = false;
    };

    auto create_nv12_textures = [&](uint32_t w, uint32_t h) {
        destroy_nv12_textures();

        y_image = create_image(device, physical_device, w, h, VK_FORMAT_R8_UNORM,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        uv_image = create_image(device, physical_device, w / 2, h / 2, VK_FORMAT_R8G8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        y_view = create_image_view(device, y_image.image, VK_FORMAT_R8_UNORM);
        uv_view = create_image_view(device, uv_image.image, VK_FORMAT_R8G8_UNORM);
        y_sampler = create_sampler(device);
        uv_sampler = create_sampler(device);

        auto cmd = begin_one_shot(device, command_pool);
        transition_image_layout(cmd, y_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition_image_layout(cmd, uv_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        end_one_shot(device, command_pool, graphics_queue, cmd);

        std::array<VkDescriptorImageInfo, 2> image_infos{};
        image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_infos[0].imageView = y_view;
        image_infos[0].sampler = y_sampler;
        image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_infos[1].imageView = uv_view;
        image_infos[1].sampler = uv_sampler;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = image_infos.data();
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptor_set;
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = image_infos.data() + 1;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);

        tex_w = w;
        tex_h = h;
        has_texture = true;
    };

    auto destroy_swapchain_resources = [&]() {
        for (auto fb : framebuffers) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
        framebuffers.clear();
        for (auto view : swapchain_image_views) {
            vkDestroyImageView(device, view, nullptr);
        }
        swapchain_image_views.clear();
        swapchain_images.clear();
    };

    auto recreate_swapchain = [&]() {
        auto [w, h] = win->size_pixels();
        while (w == 0 || h == 0) {
            win->wait_events();
            std::tie(w, h) = win->size_pixels();
        }
        vkDeviceWaitIdle(device);
        destroy_swapchain_resources();
        auto old_swapchain = swapchain;
        swapchain = VK_NULL_HANDLE;
        create_swapchain_resources(old_swapchain, swapchain, swapchain_images,
                                   swapchain_image_views, swapchain_extent);
        vkDestroySwapchainKHR(device, old_swapchain, nullptr);
        create_framebuffers(swapchain_image_views, swapchain_extent, framebuffers);
    };

    open_.store(true, std::memory_order_release);
    started_.store(true, std::memory_order_release);
    mirage::log::info("Vulkan render window started");

    uint32_t current_frame = 0;
    bool resize_needed = false;

    while (open_.load(std::memory_order_relaxed)) {
        if (!win->poll_events()) {
            open_.store(false, std::memory_order_relaxed);
            mirage::log::info("Render window closed by user");
        }
        if (win->resize_pending()) {
            resize_needed = true;
            win->clear_resize_flag();
        }

        if (!open_.load(std::memory_order_relaxed)) {
            break;
        }

        if (resize_needed) {
            recreate_swapchain();
            resize_needed = false;
        }

        bool had_frame = false;
        {
            std::lock_guard lock(frame_mutex_);
            if (pending_frame_) {
                auto& frame = *pending_frame_;
                auto fw = static_cast<uint32_t>(frame.width);
                auto fh = static_cast<uint32_t>(frame.height);
                if (fw != tex_w || fh != tex_h) {
                    vkDeviceWaitIdle(device);
                    create_nv12_textures(fw, fh);
                }
                upload_texture(device, physical_device, command_pool, graphics_queue, y_image.image,
                               frame.y_plane.data(), static_cast<uint32_t>(frame.stride_y), fw, fh,
                               1);
                upload_texture(device, physical_device, command_pool, graphics_queue,
                               uv_image.image, frame.uv_plane.data(),
                               static_cast<uint32_t>(frame.stride_uv), fw / 2, fh / 2, 2);
                pending_frame_.reset();
                had_frame = true;
            }
        }

        vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE, UINT64_MAX);

        uint32_t image_index = 0;
        auto acquire_result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                                    image_available_semaphores[current_frame],
                                                    VK_NULL_HANDLE, &image_index);
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
            continue;
        }

        vkResetFences(device, 1, &in_flight_fences[current_frame]);
        vkResetCommandBuffer(command_buffers[current_frame], 0);

        auto cmd = command_buffers[current_frame];
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        VkClearValue clear_value{};
        clear_value.color = {{0.0F, 0.0F, 0.0F, 1.0F}};

        VkRenderPassBeginInfo rp_begin{};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.renderPass = render_pass;
        rp_begin.framebuffer = framebuffers[image_index];
        rp_begin.renderArea.offset = {0, 0};
        rp_begin.renderArea.extent = swapchain_extent;
        rp_begin.clearValueCount = 1;
        rp_begin.pClearValues = &clear_value;

        vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);

        if (has_texture) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                                    &descriptor_set, 0, nullptr);
        }

        VkViewport viewport{};
        viewport.x = 0.0F;
        viewport.y = 0.0F;
        viewport.width = static_cast<float>(swapchain_extent.width);
        viewport.height = static_cast<float>(swapchain_extent.height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapchain_extent;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        if (has_texture && tex_w > 0 && tex_h > 0 && swapchain_extent.width > 0 &&
            swapchain_extent.height > 0) {
            float video_aspect = static_cast<float>(tex_w) / static_cast<float>(tex_h);
            float win_aspect = static_cast<float>(swapchain_extent.width) /
                               static_cast<float>(swapchain_extent.height);
            float dst_x;
            float dst_y;
            float dst_w;
            float dst_h;
            if (video_aspect > win_aspect) {
                dst_w = 1.0F;
                dst_h = win_aspect / video_aspect;
                dst_x = 0.0F;
                dst_y = (1.0F - dst_h) / 2.0F;
            } else {
                dst_h = 1.0F;
                dst_w = video_aspect / win_aspect;
                dst_x = (1.0F - dst_w) / 2.0F;
                dst_y = 0.0F;
            }
            std::array<float, 4> push_data = {dst_x, dst_y, dst_w, dst_h};
            vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(float) * 4, push_data.data());
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_available_semaphores[current_frame];
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_finished_semaphores[current_frame];

        vkQueueSubmit(graphics_queue, 1, &submit_info, in_flight_fences[current_frame]);

        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_finished_semaphores[current_frame];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;

        auto present_result = vkQueuePresentKHR(graphics_queue, &present_info);
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain();
        }

        if (!had_frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        current_frame = (current_frame + 1) % max_frames_in_flight;
    }

    vkDeviceWaitIdle(device);

    destroy_nv12_textures();

    for (uint32_t i = 0; i < max_frames_in_flight; ++i) {
        vkDestroySemaphore(device, render_finished_semaphores[i], nullptr);
        vkDestroySemaphore(device, image_available_semaphores[i], nullptr);
        vkDestroyFence(device, in_flight_fences[i], nullptr);
    }

    vkDestroyCommandPool(device, command_pool, nullptr);
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    vkDestroyPipeline(device, graphics_pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);

    for (auto fb : framebuffers) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    vkDestroyRenderPass(device, render_pass, nullptr);
    for (auto view : swapchain_image_views) {
        vkDestroyImageView(device, view, nullptr);
    }
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    win.reset();
    mirage::log::info("Vulkan render window thread exited");
}

}  // namespace mirage::render
