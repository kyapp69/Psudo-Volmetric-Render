/*
 * Copyright (C) 2016 Matthew Wellings
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#define MAX_LAYERS 8
//#define FORCE_VALIDATION
//#define NO_SURFACE_EXTENSIONS //Usefull for mali devices that report no surface extentions.

//BEGIN_INCLUDE(all)
#include <initializer_list>
//#include <jni.h>
//#include <errno.h>
#include <cassert>
#include <unistd.h>
#include <stdlib.h>
#include <cinttypes>
#include <string.h>

#include <stdio.h>
#include "matrix.h"
#include "models.h"
#include "btQuickprof.h"
#include "Simulation.h"
#include "log.h"

#ifdef __ANDROID__
#include <android/sensor.h>
#include <android_native_app_glue.h>
#include "stdredirect.h"
#define VK_USE_PLATFORM_ANDROID_KHR
#include "vulkan_wrapper.h"
#else
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_platform.h>
#endif

void createSecondaryBuffers(struct engine* engine);
int setupUniforms(struct engine* engine);
int setupTraditionalBlendPipeline(struct engine* engine);
int setupBlendPipeline(struct engine* engine);
int setupPeelPipeline(struct engine* engine);

/**
 * Our saved state data.
 */
struct saved_state {
    float angle;
    int32_t x;
    int32_t y;
};

/**
 * Shared state for our app.
 */
struct engine {
    struct android_app* app;

#ifdef __ANDROID__
    ASensorManager* sensorManager;
    const ASensor* accelerometerSensor;
    ASensorEventQueue* sensorEventQueue;
#else
    xcb_connection_t *xcbConnection;
    uint32_t window;
#endif
    int animating = 1;
    VkInstance vkInstance;
    VkDevice vkDevice;
    VkPhysicalDevice physicalDevice;
    VkPhysicalDeviceProperties deviceProperties;
    VkPhysicalDeviceMemoryProperties physicalDeviceMemoryProperties;
    VkCommandBuffer setupCommandBuffer;
    VkCommandBuffer renderCommandBuffer[2];
    VkCommandBuffer *secondaryCommandBuffers;
    VkImage depthImage[2];
    VkImageView depthView[2];
    VkDeviceMemory depthMemory;
    VkImage peelImages[2];
    VkImageView peelViews[2];
    VkDeviceMemory peelMemory;
    uint32_t swapchainImageCount = 0;
    VkSwapchainKHR swapchain;
    VkImage *swapChainImages;
    VkImageView *swapChainViews;
    VkFramebuffer *framebuffers;
    uint8_t *uniformMappedMemory;
    VkSemaphore presentCompleteSemaphore;
    VkRenderPass renderPass;
    VkPipelineLayout pipelineLayout;
    VkPipelineLayout peelPipelineLayout;
    VkPipelineLayout blendPipelineLayout;
    VkDescriptorSetLayout *descriptorSetLayouts;
    VkDescriptorSet sceneDescriptorSet;
    VkDescriptorSet *modelDescriptorSets;
    VkDescriptorSet identityModelDescriptorSet;
    VkDescriptorSet identitySceneDescriptorSet;
    VkDescriptorSet colourInputAttachmentDescriptorSets[2];
    VkDescriptorSet depthInputAttachmentDescriptorSets[2];
    uint32_t modelBufferValsOffset;
    VkBuffer vertexBuffer;
    VkQueue queue;
    bool vulkanSetupOK;
    int frame = 0;
    int32_t width;
    int32_t height;
    struct saved_state state;
    VkPipeline traditionalBlendPipeline;
    VkPipeline peelPipeline;
    VkPipeline firstPeelPipeline;
    VkPipeline blendPipeline;
    btClock *frameRateClock;
    Simulation *simulation;
    bool splitscreen;
    bool rebuildCommadBuffersRequired;
    VkVertexInputBindingDescription vertexInputBindingDescription;
    VkVertexInputAttributeDescription vertexInputAttributeDescription;
    VkShaderModule shdermodules[6];
    int displayLayer;
    int layerCount;
    int boxCount;

    const int NUM_SAMPLES = 1;
};

char* loadAsset(const char* filename, struct engine *pEngine, bool &ok, size_t &size)
{
    ok=false;
#ifdef __ANDROID__
    char *buffer = NULL;
    AAsset* asset = AAssetManager_open(pEngine->app->activity->assetManager, filename, AASSET_MODE_STREAMING);
    if (!asset) {
        LOGE("Cannot open asset %s", filename);
        return NULL;
    }
    size = AAsset_getLength(asset);
    if (size==0)
    {
        LOGE("Cannot open asset %s (file empty)", filename);
        return NULL;
    }
    buffer = (char*)malloc(size);
    int bytesRead = AAsset_read(asset, buffer, size);
    if (bytesRead < 0) {
        LOGE("Cannot read asset %s", filename);
        return NULL;
    }
    AAsset_close(asset);
    ok=true;
    LOGI("File %s read %d bytes.", filename, bytesRead);
    return buffer;
#else
    char path[100];
    strcpy(path, "../assets/");
    strcat(path, filename);
    size_t retval;
    void *fileContents;

    FILE *fileHandle = fopen(path, "rb");
    if (!fileHandle) return NULL;

    fseek(fileHandle, 0L, SEEK_END);
    size = ftell(fileHandle);

    fseek(fileHandle, 0L, SEEK_SET);

    fileContents = malloc(size);
    retval = fread(fileContents, size, 1, fileHandle);
    assert(retval == 1);

    ok=true;
    return (char*)fileContents;
#endif
}

void updateColours(struct engine* engine);

/**
 * Initialize an EGL context for the current display.
 */
static int engine_init_display(struct engine* engine) {
    // initialize Vulkan

    LOGI ("Initializing Vulkan\n");

#ifdef __ANDROID__
#ifdef FORCE_VALIDATION
    //Redirect stdio to android log so vulkan validation layer output is not lost (should use the vulkan extension for this but some drivers don't support it).
    //Redirect code from https://codelab.wordpress.com/2014/11/03/how-to-use-standard-output-streams-for-logging-in-android-apps/
    /* make stdout line-buffered and stderr unbuffered */
    redirectStdIO();
    {
        //Copy vk_layer_settings.txt to a place where it can be found by the Vulkan loader.
        bool ok;
        size_t size;
        char *vk_layer_settings = loadAsset("vk_layer_settings.txt", engine, ok, size);
        if (ok) {
            char filenamepath[1024] = "";
            char *filename= "/vk_layer_settings.txt";
            strcat(filenamepath, engine->app->activity->internalDataPath);
            strcat(filenamepath, filename);
            LOGI("filenamepath: %s\n", filenamepath);
            FILE *write_ptr;
            write_ptr = fopen(filenamepath,"wb");
            fwrite(vk_layer_settings, size, 1, write_ptr);
        }
    }

    //Set the working directory to where we just put the vk_layer_settings.txt file.
    char oldcwd[1024];
    char cwd[1024];
    if (getcwd(oldcwd, sizeof(oldcwd)) != NULL)
        LOGI("Current working dir: %s\n", oldcwd);

    LOGI("internalDataPath: %s\n", engine->app->activity->internalDataPath);

    chdir(engine->app->activity->internalDataPath);

    if (getcwd(cwd, sizeof(cwd)) != NULL)
        LOGI("Current working dir: %s\n", cwd);
#endif

    //We will put the working directory back to oldcwd later.
    //Use glload to load the vulkan loader and setup funcitions.
    if (InitVulkan()==0)
    {
        LOGI("InitVulkan() failed");
        return -1;
    }
#endif

    VkResult res;

    uint32_t availableLayerCount =0;
    res = vkEnumerateInstanceLayerProperties(&availableLayerCount, NULL);
    LOGI("There are %d instance layers avalible\n", availableLayerCount);
    VkLayerProperties availableLayers[availableLayerCount];
    res = vkEnumerateInstanceLayerProperties(&availableLayerCount, availableLayers);
    for (int i =0; i < availableLayerCount; i++)
    {
        LOGI("%s: %s\n", availableLayers[i].layerName, availableLayers[i].description);
    }

    const char *enabledLayerNames[] = {
            //List any layers you want to enable here.
        "VK_LAYER_LUNARG_core_validation",
        "VK_LAYER_LUNARG_swapchain",
        "VK_LAYER_LUNARG_device_limits",
        "VK_LAYER_LUNARG_image",
        "VK_LAYER_LUNARG_object_tracker",
        "VK_LAYER_LUNARG_parameter_validation",
        "VK_LAYER_GOOGLE_threading",
        "VK_LAYER_GOOGLE_unique_objects"
    };

    const char *enabledInstanceExtensionNames[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
    #ifdef __ANDROID__
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    #else
            VK_KHR_XCB_SURFACE_EXTENSION_NAME
    #endif
    };

    VkApplicationInfo app_info;
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pNext = NULL;
    app_info.pApplicationName = "My Test App";
    app_info.applicationVersion = 1;
    app_info.pEngineName = "My Test App Engine";
    app_info.engineVersion = 1;
    app_info.apiVersion = VK_MAKE_VERSION(1, 0, 2);

    //Initialize the VkInstanceCreateInfo structure
    VkInstanceCreateInfo inst_info;
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pNext = NULL;
    inst_info.flags = 0;
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledExtensionCount = 2;
#ifdef NO_SURFACE_EXTENSIONS
    inst_info.enabledExtensionCount = 0;
#endif
    inst_info.ppEnabledExtensionNames = enabledInstanceExtensionNames;
#ifdef FORCE_VALIDATION
    inst_info.enabledLayerCount = 8;
#else
    inst_info.enabledLayerCount = 0;
#endif
    inst_info.ppEnabledLayerNames = enabledLayerNames;

    res = vkCreateInstance(&inst_info, NULL, &engine->vkInstance);
    if (res == VK_ERROR_INCOMPATIBLE_DRIVER) {
        LOGE ("vkCreateInstance returned VK_ERROR_INCOMPATIBLE_DRIVER\n");
        return -1;
    } else if (res != VK_SUCCESS) {
        LOGE ("vkCreateInstance returned error %d\n", res);
        return -1;
    }
    LOGI ("Vulkan instance created\n");

    uint32_t deviceBufferSize=0;
    res = vkEnumeratePhysicalDevices(engine->vkInstance, &deviceBufferSize, NULL);
    LOGI ("GPU Count: %i\n", deviceBufferSize);
    if (deviceBufferSize==0)
    {
        LOGE("No Vulkan device");
#ifdef __ANDROID__
        ANativeActivity_finish(engine->app->activity);
#endif
        return -1;
    }
    VkPhysicalDevice physicalDevices[deviceBufferSize];
    res = vkEnumeratePhysicalDevices(engine->vkInstance, &deviceBufferSize, physicalDevices);
    if (res == VK_ERROR_INITIALIZATION_FAILED) {
        LOGE ("vkEnumeratePhysicalDevices returned VK_ERROR_INITIALIZATION_FAILED for GPU 0.\n");
        return -1;
    }else if (res != VK_SUCCESS) {
        LOGE ("vkEnumeratePhysicalDevices returned error.\n");
        return -1;
    }
    engine->physicalDevice=physicalDevices[0];

    VkSurfaceKHR surface;
#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR instInfo;
    instInfo.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    instInfo.pNext=NULL;
    instInfo.window=engine->app->window;

    res =  vkCreateAndroidSurfaceKHR(engine->vkInstance, &instInfo, NULL, &surface);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateAndroidSurfaceKHR returned error.\n");
        return -1;
    }
#else
    VkXcbSurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = NULL;
    createInfo.connection = engine->xcbConnection;
    createInfo.window = engine->window;
    res = vkCreateXcbSurfaceKHR(engine->vkInstance, &createInfo, NULL, &surface);
    if (res != VK_SUCCESS) {
      printf ("vkCreateXcbSurfaceKHR returned error.\n");
      return -1;
    }
#endif


    LOGI ("Vulkan surface created\n");

    vkGetPhysicalDeviceMemoryProperties(engine->physicalDevice, &engine->physicalDeviceMemoryProperties);
    LOGI ("There are %d memory types.\n", engine->physicalDeviceMemoryProperties.memoryTypeCount);

    VkDeviceQueueCreateInfo deviceQueueCreateInfo;
    deviceQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    deviceQueueCreateInfo.flags = 0;
    deviceQueueCreateInfo.pNext = NULL;
    deviceQueueCreateInfo.queueCount = 1;
    float queuePriorities[1] = {1.0};
    deviceQueueCreateInfo.pQueuePriorities = queuePriorities;

    uint32_t queueCount=0;
    //We are only using the first physical device:
    vkGetPhysicalDeviceQueueFamilyProperties(engine->physicalDevice, &queueCount, NULL);
    LOGI ("%i PhysicalDeviceQueueFamily(ies).\n", queueCount);

    VkQueueFamilyProperties queueFamilyProperties[queueCount];
    vkGetPhysicalDeviceQueueFamilyProperties(engine->physicalDevice, &queueCount, queueFamilyProperties);
    int found = 0;
    unsigned int i = 0;
    VkBool32 supportsPresent;
    for (; i < queueCount; i++) {
        if (queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            LOGI ("PhysicalDeviceQueueFamily %i has property VK_QUEUE_GRAPHICS_BIT.\n", i);
            vkGetPhysicalDeviceSurfaceSupportKHR(engine->physicalDevice, i, surface, &supportsPresent);
            if (supportsPresent) {
                deviceQueueCreateInfo.queueFamilyIndex = i;
                found = 1;
                break;
            }
        }
    }
    if (found==0) {
        LOGE ("Error: A suitable queue family has not been found.\n");
        return -1;
    }

    availableLayerCount =0;
    res = vkEnumerateDeviceLayerProperties(engine->physicalDevice, &availableLayerCount, NULL);
    LOGI("There are %d device layers avalible\n", availableLayerCount);
    availableLayers[availableLayerCount];
    if (availableLayerCount>0)
    res = vkEnumerateDeviceLayerProperties(engine->physicalDevice, &availableLayerCount,
                                           availableLayers);
    for (int i =0; i < availableLayerCount; i++)
    {
        LOGI("%s: %s\n", availableLayers[i].layerName, availableLayers[i].description);
    }

    const char *enabledDeviceExtensionNames[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = NULL;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &deviceQueueCreateInfo;
    dci.enabledExtensionCount = 1;
#ifdef NO_SURFACE_EXTENSIONS
    dci.enabledExtensionCount = 0;
#endif
    dci.ppEnabledExtensionNames = enabledDeviceExtensionNames;
    dci.pEnabledFeatures = NULL;
#ifdef FORCE_VALIDATION
    dci.enabledLayerCount = 8;
#else
    dci.enabledLayerCount = 0;
#endif
    dci.ppEnabledLayerNames = enabledLayerNames;

    res = vkCreateDevice(engine->physicalDevice, &dci, NULL, &engine->vkDevice);
    if (res == VK_ERROR_INITIALIZATION_FAILED) {
        LOGE ("vkCreateDevice returned VK_ERROR_INITIALIZATION_FAILED for GPU 0.\n");
        return -1;
    }else if (res != VK_SUCCESS) {
        LOGE ("vkCreateDevice returned error %d.\n", res);
        return -1;
    }
    LOGI("vkCreateDevice successful");



    //Setup the swapchain
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(engine->physicalDevice, surface, &formatCount, NULL);
    VkSurfaceFormatKHR formats[formatCount];
    vkGetPhysicalDeviceSurfaceFormatsKHR(engine->physicalDevice, surface, &formatCount, formats);

    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(engine->physicalDevice, surface, &presentModeCount, NULL);
    VkPresentModeKHR presentModes[presentModeCount];
    vkGetPhysicalDeviceSurfacePresentModesKHR(engine->physicalDevice, surface, &presentModeCount, presentModes);

    VkSurfaceCapabilitiesKHR surfCapabilities;
    res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(engine->physicalDevice, surface, &surfCapabilities);

    VkExtent2D swapChainExtent;
    // width and height are either both -1, or both not -1.
    if (surfCapabilities.currentExtent.width == (uint32_t)-1) {
        // If the surface size is undefined, the size is set to
        // the size of the images requested.
//        swapChainExtent.width = 800;
//        swapChainExtent.height = 600;
        LOGE("Swapchain size is (-1, -1)\n");
        return -1;
    } else {
        // If the surface size is defined, the swap chain size must match
        swapChainExtent = surfCapabilities.currentExtent;
        LOGI("Swapchain size is (%d, %d)\n", swapChainExtent.width, swapChainExtent.height);
        engine->width=swapChainExtent.width;
        engine->height=swapChainExtent.height;
    }

    // If the format list includes just one entry of VK_FORMAT_UNDEFINED,
    // the surface has no preferred format.  Otherwise, at least one
    // supported format will be returned.
    VkFormat format;
    if (formatCount == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        format = VK_FORMAT_R8G8B8A8_UNORM;
    } else {
        assert(formatCount >= 1);
        format = formats[0].format;
    }
    LOGI("Using format %d\n", format);

    uint32_t desiredNumberOfSwapChainImages = surfCapabilities.minImageCount + 1;
    if ((surfCapabilities.maxImageCount > 0) &&
        (desiredNumberOfSwapChainImages > surfCapabilities.maxImageCount)) {
        // Application must settle for fewer images than desired:
        desiredNumberOfSwapChainImages = surfCapabilities.maxImageCount;
    }
    LOGI("Asking for %d SwapChainImages\n", desiredNumberOfSwapChainImages);

    VkSurfaceTransformFlagBitsKHR preTransform;
    if (surfCapabilities.supportedTransforms &
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
        preTransform = surfCapabilities.currentTransform;
    }
    LOGI("Using preTransform %d\n", preTransform);

    VkSwapchainCreateInfoKHR swapCreateInfo;
    swapCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapCreateInfo.pNext = NULL;
    swapCreateInfo.surface = surface;
    swapCreateInfo.minImageCount = desiredNumberOfSwapChainImages;
    swapCreateInfo.imageFormat = format;
    swapCreateInfo.imageExtent=swapChainExtent;
    //swapCreateInfo.imageExtent.width = width; //Should match window size
    //swapCreateInfo.imageExtent.height = height;
    swapCreateInfo.preTransform = preTransform;
    swapCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    swapCreateInfo.imageArrayLayers = 1;
    swapCreateInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapCreateInfo.oldSwapchain = VK_NULL_HANDLE;
    swapCreateInfo.clipped = VK_TRUE;
    swapCreateInfo.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    swapCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapCreateInfo.queueFamilyIndexCount = 0;
    swapCreateInfo.pQueueFamilyIndices = NULL;

    vkCreateSwapchainKHR(engine->vkDevice, &swapCreateInfo, NULL, &engine->swapchain);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateSwapchainKHR returned error.\n");
        return -1;
    }
    LOGI("Swapchain created");

    //Setup Command buffers
    VkCommandPool commandPool;
    VkCommandPoolCreateInfo commandPoolCreateInfo = {};
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.pNext = NULL;
    commandPoolCreateInfo.queueFamilyIndex = deviceQueueCreateInfo.queueFamilyIndex;
    commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    res = vkCreateCommandPool(engine->vkDevice, &commandPoolCreateInfo, NULL, &commandPool);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateCommandPool returned error.\n");
        return -1;
    }

    VkCommandBufferAllocateInfo commandBufferAllocateInfo = {};
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.pNext = NULL;
    commandBufferAllocateInfo.commandPool = commandPool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = 3;

    VkCommandBuffer commandBuffers[3];
    res = vkAllocateCommandBuffers(engine->vkDevice, &commandBufferAllocateInfo, commandBuffers);
    if (res != VK_SUCCESS) {
        LOGE ("vkAllocateCommandBuffers returned error.\n");
        return -1;
    }

    engine->setupCommandBuffer=commandBuffers[0];
    engine->renderCommandBuffer[0]=commandBuffers[1];
    engine->renderCommandBuffer[1]=commandBuffers[2];

    LOGI("Command buffers created");

    VkCommandBufferBeginInfo commandBufferBeginInfo = {};
    commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    commandBufferBeginInfo.pNext = NULL;
    commandBufferBeginInfo.flags = 0;
    commandBufferBeginInfo.pInheritanceInfo = NULL;

    res = vkBeginCommandBuffer(engine->setupCommandBuffer, &commandBufferBeginInfo);
    if (res != VK_SUCCESS) {
        LOGE ("vkBeginCommandBuffer returned error.\n");
        return -1;
    }

    vkGetDeviceQueue(engine->vkDevice, deviceQueueCreateInfo.queueFamilyIndex, 0, &engine->queue);

    vkGetSwapchainImagesKHR(engine->vkDevice, engine->swapchain, &engine->swapchainImageCount, NULL);
    engine->swapChainImages = new VkImage[engine->swapchainImageCount];
    res = vkGetSwapchainImagesKHR(engine->vkDevice, engine->swapchain, &engine->swapchainImageCount, engine->swapChainImages);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateSwapchainKHR returned error.\n");
        return -1;
    }
    printf ("swapchainImageCount %d.\n",engine->swapchainImageCount);

    engine->swapChainViews=(VkImageView*)malloc(sizeof (VkImageView) *engine->swapchainImageCount);
    for (uint32_t i = 0; i < engine->swapchainImageCount; i++) {
        LOGI ("Setting up swapChainView %d.\n",i);
        VkImageViewCreateInfo color_image_view = {};
        color_image_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        color_image_view.pNext = NULL;
        color_image_view.format = format;
        color_image_view.components.r = VK_COMPONENT_SWIZZLE_R;
        color_image_view.components.g = VK_COMPONENT_SWIZZLE_G;
        color_image_view.components.b = VK_COMPONENT_SWIZZLE_B;
        color_image_view.components.a = VK_COMPONENT_SWIZZLE_A;
        color_image_view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        color_image_view.subresourceRange.baseMipLevel = 0;
        color_image_view.subresourceRange.levelCount = 1;
        color_image_view.subresourceRange.baseArrayLayer = 0;
        color_image_view.subresourceRange.layerCount = 1;
        color_image_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        color_image_view.flags = 0;
        color_image_view.image = engine->swapChainImages[i];

        VkImageMemoryBarrier imageMemoryBarrier;
        imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imageMemoryBarrier.pNext = NULL;
        imageMemoryBarrier.image = engine->swapChainImages[i];
        imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
        imageMemoryBarrier.subresourceRange.levelCount = 1;
        imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
        imageMemoryBarrier.subresourceRange.layerCount = 1;
        imageMemoryBarrier.srcQueueFamilyIndex=0;
        imageMemoryBarrier.dstQueueFamilyIndex=0;
        //imageMemoryBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        //imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        //imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        imageMemoryBarrier.srcAccessMask = 0;
        imageMemoryBarrier.dstAccessMask = 0;

        // Put barrier on top
        VkPipelineStageFlags srcStageFlags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags destStageFlags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        // Put barrier inside setup command buffer
        vkCmdPipelineBarrier(engine->setupCommandBuffer, srcStageFlags, destStageFlags, 0,
                             0, NULL, 0, NULL, 1, &imageMemoryBarrier);

        res = vkCreateImageView(engine->vkDevice, &color_image_view, NULL, &engine->swapChainViews[i]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateImageView returned error.\n");
            return -1;
        }
    }
    LOGI ("swapchainImageCount %d.\n", engine->swapchainImageCount);

    //Setup the depth buffer:
    const VkFormat depth_format = VK_FORMAT_D24_UNORM_S8_UINT;

    VkImageCreateInfo imageCreateInfo;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(engine->physicalDevice, depth_format, &props);
    if (! (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        LOGE ("depth_format %d Unsupported.\n", depth_format);
        return -1;
    }

    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = NULL;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = depth_format;
    imageCreateInfo.extent.width = swapChainExtent.width;
    imageCreateInfo.extent.height = swapChainExtent.height;
    imageCreateInfo.extent.depth = 1;
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.queueFamilyIndexCount = 0;
    imageCreateInfo.pQueueFamilyIndices = NULL;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.flags = 0;

    //First try using lazy memory:
    imageCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

    VkMemoryRequirements memoryRequirements;
    //Create images for depth buffers
    for (int i=0; i<2; i++)
    {
        res = vkCreateImage(engine->vkDevice, &imageCreateInfo, NULL, &engine->depthImage[i]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateImage returned error while creating depth buffer.\n");
            return -1;
        }
    }

    vkGetImageMemoryRequirements(engine->vkDevice, engine->depthImage[0], &memoryRequirements);

    found = false;
    uint32_t typeBits = memoryRequirements.memoryTypeBits;
    uint32_t typeIndex;
    VkFlags requirements_mask = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    for (typeIndex = 0; typeIndex < engine->physicalDeviceMemoryProperties.memoryTypeCount; typeIndex++) {
        if ((typeBits & 1) == 1)//Check last bit;
        {
            if ((engine->physicalDeviceMemoryProperties.memoryTypes[typeIndex].propertyFlags & requirements_mask) == requirements_mask)
            {
                found=true;
                break;
            }
        }
        typeBits >>= 1;
    }
    if (found)
        LOGI("Using lazily allocated memory & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT for the depth buffers.");
    else
    {
        LOGI("Not using lazily allocated memory for the depth buffers.");
        //Either there was no lazily allocated memory or it cannot be used for the depth buffers (because no memory type with VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT was in the memoryTypeBits mask).
        imageCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        for (int i=0; i<2; i++)
            vkDestroyImage(engine->vkDevice, engine->depthImage[i], NULL);
        //Create new images for depth buffers
        for (int i=0; i<2; i++)
        {
            res = vkCreateImage(engine->vkDevice, &imageCreateInfo, NULL, &engine->depthImage[i]);
            if (res != VK_SUCCESS) {
                LOGE ("vkCreateImage returned error while creating depth buffer.\n");
                return -1;
            }
        }
        vkGetImageMemoryRequirements(engine->vkDevice, engine->depthImage[0], &memoryRequirements);
        typeBits = memoryRequirements.memoryTypeBits;
        //Get the index of the first set bit:
        for (typeIndex = 0; typeIndex < 32; typeIndex++) {
            if ((typeBits & 1) == 1)//Check last bit;
                break;
            typeBits >>= 1;
        }
    }

    VkDeviceSize imageoffset = 0;
    if (memoryRequirements.alignment > 1 && memoryRequirements.size % memoryRequirements.alignment != 0)
        imageoffset = memoryRequirements.size + (memoryRequirements.alignment-(memoryRequirements.size % memoryRequirements.alignment));
    else
        imageoffset = memoryRequirements.size;

    LOGI("MemoryRequirements.size: %" PRIu64 " memoryRequirements.alignment: %" PRIu64 " imageoffset: %" PRIu64 ".", memoryRequirements.size, memoryRequirements.alignment, imageoffset);
    VkMemoryAllocateInfo memAllocInfo;
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.pNext = NULL;
    memAllocInfo.allocationSize = imageoffset + memoryRequirements.size;
    memAllocInfo.memoryTypeIndex = typeIndex;

    //Allocate memory
    res = vkAllocateMemory(engine->vkDevice, &memAllocInfo, NULL, &engine->depthMemory);
    if (res != VK_SUCCESS) {
        LOGE ("vkAllocateMemory returned error while creating depth buffer.\n");
        return -1;
    }

    for (int i=0; i<2; i++)
    {
        //Bind memory
        res = vkBindImageMemory(engine->vkDevice, engine->depthImage[i], engine->depthMemory, imageoffset*i);
        if (res != VK_SUCCESS) {
            LOGE ("vkBindImageMemory returned error while creating depth buffer. %d\n", res);
            return -1;
        }

        VkImageMemoryBarrier imageMemoryBarrier;
        imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        imageMemoryBarrier.pNext = NULL;
        imageMemoryBarrier.image = engine->depthImage[i];
        imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
        imageMemoryBarrier.subresourceRange.levelCount = 1;
        imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
        imageMemoryBarrier.subresourceRange.layerCount = 1;
        imageMemoryBarrier.srcQueueFamilyIndex=0;
        imageMemoryBarrier.dstQueueFamilyIndex=0;
        imageMemoryBarrier.srcAccessMask = 0;
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        // Put barrier on top
        VkPipelineStageFlags srcStageFlags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags destStageFlags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        // Put barrier inside setup command buffer
        vkCmdPipelineBarrier(engine->setupCommandBuffer, srcStageFlags, destStageFlags, 0,
                             0, NULL, 0, NULL, 1, &imageMemoryBarrier);


        //Create image view
        VkImageViewCreateInfo view_info;
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.pNext = NULL;
        view_info.image = engine->depthImage[i];
        view_info.format = depth_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.components.r = VK_COMPONENT_SWIZZLE_R;
        view_info.components.g = VK_COMPONENT_SWIZZLE_G;
        view_info.components.b = VK_COMPONENT_SWIZZLE_B;
        view_info.components.a = VK_COMPONENT_SWIZZLE_A;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.flags = 0;

        res = vkCreateImageView(engine->vkDevice, &view_info, NULL, &engine->depthView[i]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateImageView returned error while creating depth buffer. %d\n", res);
            return -1;
        }
    }
    LOGI("Depth buffers created");

    //Setup the peel buffers:
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = NULL;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = format;
    imageCreateInfo.extent.width = swapChainExtent.width;
    imageCreateInfo.extent.height = swapChainExtent.height;
    imageCreateInfo.extent.depth = 1;
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.queueFamilyIndexCount = 0;
    imageCreateInfo.pQueueFamilyIndices = NULL;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.flags = 0;

    //First try using lazy memory:
    imageCreateInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

    //Create images for peel buffer

    for (int i=0; i<2; i++)
    {
        res = vkCreateImage(engine->vkDevice, &imageCreateInfo, NULL, &engine->peelImages[i]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateImage returned error while creating peel buffer.\n");
            return -1;
        }
    }

    memoryRequirements.size=0;
    vkGetImageMemoryRequirements(engine->vkDevice, engine->peelImages[0], &memoryRequirements);

    found = false;
    typeBits = memoryRequirements.memoryTypeBits;
    requirements_mask = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    for (typeIndex = 0; typeIndex < engine->physicalDeviceMemoryProperties.memoryTypeCount; typeIndex++) {
        if ((typeBits & 1) == 1)//Check last bit;
        {
            if ((engine->physicalDeviceMemoryProperties.memoryTypes[typeIndex].propertyFlags & requirements_mask) == requirements_mask)
            {
                found=true;
                break;
            }
        }
        typeBits >>= 1;
    }
    if (found)
        LOGI("Using lazily allocated memory & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT for the peel buffer.");
    else
    {
        LOGI("Not using lazily allocated memory for the peel buffer.");
        imageCreateInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        for (int i=0; i<2; i++)
            vkDestroyImage(engine->vkDevice, engine->peelImages[i], NULL);
        //Create image for peel buffer
        for (int i=0; i<2; i++)
        {
            res = vkCreateImage(engine->vkDevice, &imageCreateInfo, NULL, &engine->peelImages[i]);
            if (res != VK_SUCCESS) {
                LOGE ("vkCreateImage returned error while creating peel buffer.\n");
                return -1;
            }
        }
        vkGetImageMemoryRequirements(engine->vkDevice, engine->peelImages[0], &memoryRequirements);
        typeBits = memoryRequirements.memoryTypeBits;
        //Get the index of the first set bit:
        for (typeIndex = 0; typeIndex < 32; typeIndex++) {
            if ((typeBits & 1) == 1)//Check last bit;
                break;
            typeBits >>= 1;
        }
    }

    imageoffset = 0;
    if (memoryRequirements.alignment > 1 && memoryRequirements.size % memoryRequirements.alignment != 0)
        imageoffset = memoryRequirements.size + (memoryRequirements.alignment-(memoryRequirements.size % memoryRequirements.alignment));
    else
        imageoffset = memoryRequirements.size;

    LOGI("MemoryRequirements.size: %" PRIu64 " memoryRequirements.alignment: %" PRIu64 " imageoffset: %" PRIu64 ".", memoryRequirements.size, memoryRequirements.alignment, imageoffset);

    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.pNext = NULL;
    memAllocInfo.allocationSize = imageoffset + memoryRequirements.size;
    memAllocInfo.memoryTypeIndex = typeIndex;

    //Allocate memory
    res = vkAllocateMemory(engine->vkDevice, &memAllocInfo, NULL, &engine->peelMemory);
    if (res != VK_SUCCESS) {
        LOGE ("vkAllocateMemory returned error while creating peel buffer.\n");
        return -1;
    }

    for (int i=0; i<2; i++)
    {
        //Bind memory
        res = vkBindImageMemory(engine->vkDevice, engine->peelImages[i], engine->peelMemory, imageoffset*i);
        if (res != VK_SUCCESS) {
            LOGE ("vkBindImageMemory returned error while creating peel buffer. %d\n", res);
            return -1;
        }

        VkImageMemoryBarrier imageMemoryBarrier;
        imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imageMemoryBarrier.pNext = NULL;
        imageMemoryBarrier.image = engine->peelImages[i];
        imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
        imageMemoryBarrier.subresourceRange.levelCount = 1;
        imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
        imageMemoryBarrier.subresourceRange.layerCount = 1;
        imageMemoryBarrier.srcQueueFamilyIndex = 0;
        imageMemoryBarrier.dstQueueFamilyIndex = 0;
        imageMemoryBarrier.srcAccessMask = 0;
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Put barrier on top
        VkPipelineStageFlags srcStageFlags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags destStageFlags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        // Put barrier inside setup command buffer
        vkCmdPipelineBarrier(engine->setupCommandBuffer, srcStageFlags, destStageFlags, 0,
                             0, NULL, 0, NULL, 1, &imageMemoryBarrier);


        //Create image view
        VkImageViewCreateInfo view_info;
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.pNext = NULL;
        view_info.image = engine->peelImages[i];
        view_info.format = format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.components.r = VK_COMPONENT_SWIZZLE_R;
        view_info.components.g = VK_COMPONENT_SWIZZLE_G;
        view_info.components.b = VK_COMPONENT_SWIZZLE_B;
        view_info.components.a = VK_COMPONENT_SWIZZLE_A;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.flags = 0;

        res = vkCreateImageView(engine->vkDevice, &view_info, NULL, &engine->peelViews[i]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateImageView returned error while creating peel buffer. %d\n", res);
            return -1;
        }
        LOGI("Peel image created");
    }
    res = vkEndCommandBuffer(engine->setupCommandBuffer);
    if (res != VK_SUCCESS) {
        LOGE ("vkEndCommandBuffer returned error %d.\n", res);
        return -1;
    }
    //Submit the setup command buffer
    VkSubmitInfo submitInfo[1];
    submitInfo[0].pNext = NULL;
    submitInfo[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo[0].waitSemaphoreCount = 0;
    submitInfo[0].pWaitSemaphores = NULL;
    submitInfo[0].pWaitDstStageMask = NULL;
    submitInfo[0].commandBufferCount = 1;
    submitInfo[0].pCommandBuffers = &engine->setupCommandBuffer;
    submitInfo[0].signalSemaphoreCount = 0;
    submitInfo[0].pSignalSemaphores = NULL;

    res = vkQueueSubmit(engine->queue, 1, submitInfo, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        LOGE ("vkQueueSubmit returned error %d.\n", res);
        return -1;
    }

    res = vkQueueWaitIdle(engine->queue);
    if (res != VK_SUCCESS) {
        LOGE ("vkQueueWaitIdle returned error %d.\n", res);
        return -1;
    }

    engine->secondaryCommandBuffers=new VkCommandBuffer[engine->swapchainImageCount*(MAX_LAYERS*2+1)];
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    commandBufferAllocateInfo.commandBufferCount = engine->swapchainImageCount*(MAX_LAYERS*2+1);

    LOGI ("Creating %d secondary command buffers.\n", engine->swapchainImageCount*(MAX_LAYERS*2+1));
    res = vkAllocateCommandBuffers(engine->vkDevice, &commandBufferAllocateInfo, engine->secondaryCommandBuffers);
    if (res != VK_SUCCESS) {
        LOGE ("vkAllocateCommandBuffers returned error.\n");
        return -1;
    }

    //Setup the renderpass:
    VkAttachmentDescription attachments[5];
    attachments[0].format = format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].flags = 0;
    attachments[1].format = depth_format;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].flags = 0;
    attachments[2].format = format;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[2].flags = 0;
    attachments[3].format = depth_format;
    attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[3].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[3].flags = 0;    
    attachments[4].format = format;
    attachments[4].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[4].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[4].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[4].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[4].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[4].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[4].flags = 0;

    VkAttachmentReference color_reference;
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_attachment_reference[2];
    depth_attachment_reference[0].attachment = 1;
    depth_attachment_reference[0].layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_attachment_reference[1].attachment = 3;
    depth_attachment_reference[1].layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_inputattachment_reference[2];
    depth_inputattachment_reference[0].attachment = 1;
    depth_inputattachment_reference[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth_inputattachment_reference[1].attachment = 3;
    depth_inputattachment_reference[1].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference peelcolor_attachment_reference[2];
    peelcolor_attachment_reference[0].attachment = 2;
    peelcolor_attachment_reference[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    peelcolor_attachment_reference[1].attachment = 4;
    peelcolor_attachment_reference[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference peelcolor_inputattachment_reference[2];
    peelcolor_inputattachment_reference[0].attachment = 2;
    peelcolor_inputattachment_reference[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    peelcolor_inputattachment_reference[1].attachment = 4;
    peelcolor_inputattachment_reference[1].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    uint32_t colour_attachment = 0;
    uint32_t depth_attachment[2] = {1, 3};
    uint32_t peel_attachment[2] = {2, 4};

    uint subpassCount = MAX_LAYERS*2+1;
    uint subpassDependencyCount=(subpassCount*(subpassCount-1))/2;
    VkSubpassDependency subpassDependencies[subpassDependencyCount];
    VkSubpassDescription subpasses[subpassCount];
    subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].flags = 0;
    subpasses[0].inputAttachmentCount = 0;
    subpasses[0].pInputAttachments = NULL;
    subpasses[0].colorAttachmentCount = 1;
    subpasses[0].pColorAttachments = &color_reference;
    subpasses[0].pResolveAttachments = NULL;
    subpasses[0].pDepthStencilAttachment = &depth_attachment_reference[0];
    subpasses[0].preserveAttachmentCount = 3;
    uint32_t PreserveAttachments[3] = {peel_attachment[0],peel_attachment[1], depth_attachment[1]};
    subpasses[0].pPreserveAttachments = PreserveAttachments;

    for (int i =0; i<MAX_LAYERS; i++)
    {

        uint32_t *PreserveAttachments = new uint32_t[3];  //This will leak
        subpasses[i * 2 + 1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpasses[i * 2 + 1].flags = 0;
        subpasses[i * 2 + 1].inputAttachmentCount = (i==0) ? 0 : 1;
        subpasses[i * 2 + 1].pInputAttachments = &depth_inputattachment_reference[!(i%2)];
        subpasses[i * 2 + 1].colorAttachmentCount = 1;
        subpasses[i * 2 + 1].pColorAttachments = &peelcolor_attachment_reference[i%2];
        subpasses[i * 2 + 1].pResolveAttachments = NULL;
        subpasses[i * 2 + 1].pDepthStencilAttachment = &depth_attachment_reference[i%2];
        subpasses[i * 2 + 1].preserveAttachmentCount = 3;
        PreserveAttachments[0] = peel_attachment[0];
        PreserveAttachments[1] = peel_attachment[1];
        PreserveAttachments[2] = depth_attachment[!(i%2)];
        subpasses[i * 2 + 1].pPreserveAttachments = PreserveAttachments;

        subpasses[i * 2 + 2].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpasses[i * 2 + 2].flags = 0;
        subpasses[i * 2 + 2].inputAttachmentCount = 4;
        VkAttachmentReference *inputAttachments = new VkAttachmentReference[4];  //This will leak
        inputAttachments[0] = peelcolor_inputattachment_reference[i%2];
        inputAttachments[1] = depth_inputattachment_reference[(i%2)];
        inputAttachments[2] = depth_inputattachment_reference[!(i%2)];
        inputAttachments[3] = peelcolor_inputattachment_reference[!(i%2)];
        subpasses[i * 2 + 2].pInputAttachments = inputAttachments;
        subpasses[i * 2 + 2].colorAttachmentCount = 1;
        subpasses[i * 2 + 2].pColorAttachments = &color_reference;
        subpasses[i * 2 + 2].pResolveAttachments = NULL;
        subpasses[i * 2 + 2].pDepthStencilAttachment = NULL;
        subpasses[i * 2 + 2].preserveAttachmentCount = 4;
        PreserveAttachments = new uint32_t[4];  //This will leak
        PreserveAttachments[0] = peel_attachment[0];
        PreserveAttachments[1] = peel_attachment[1];
        PreserveAttachments[2] = depth_attachment[0];
        PreserveAttachments[3] = depth_attachment[1];
        subpasses[i * 2 + 2].pPreserveAttachments = PreserveAttachments;
        LOGI("peel %d subpasses %d and %d pDepthStencilAttachment %d pInputAttachments %d", i, i * 2 + 1, i * 2 + 2, i%2, !(i%2));
    }

    //For simplisity every subpass will depend on all subpasses before it in the same way:
    int subpassDependencyIndex=0;
    for (int subpass=1; subpass<subpassCount; subpass++)
    {
        for (int dependantSubpass=0; dependantSubpass<subpass; dependantSubpass++)
        {
//            LOGI("Creating subpassDependency %d srcSubpass=%d dstSubpass=%d", subpassDependencyIndex, dependantSubpass, subpass);
            subpassDependencies[subpassDependencyIndex].srcSubpass = dependantSubpass;
            subpassDependencies[subpassDependencyIndex].dstSubpass = subpass;
//            subpassDependencies[subpassDependencyIndex].dstSubpass = (subpass<subpassCount) ? subpass : VK_SUBPASS_EXTERNAL;
            subpassDependencies[subpassDependencyIndex].srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
            subpassDependencies[subpassDependencyIndex].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
            subpassDependencies[subpassDependencyIndex].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            subpassDependencies[subpassDependencyIndex].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            subpassDependencies[subpassDependencyIndex].dependencyFlags = 0;
            subpassDependencyIndex++;
        }
    }
    assert(subpassDependencyIndex==subpassDependencyCount);

    LOGI("Creating renderpass %d subpasses %d subpassDependencies", subpassCount, subpassDependencyCount);
    VkRenderPassCreateInfo rp_info;
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.pNext = NULL;
    rp_info.flags=0;
    rp_info.attachmentCount = 5;
    rp_info.pAttachments = attachments;
    rp_info.subpassCount = subpassCount;
    rp_info.pSubpasses = subpasses;
    rp_info.dependencyCount = subpassDependencyCount;
    rp_info.pDependencies = subpassDependencies;
    res = vkCreateRenderPass(engine->vkDevice, &rp_info, NULL, &engine->renderPass);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateRenderPass returned error. %d\n", res);
        return -1;
    }

    LOGI("Renderpass created");

    vkGetPhysicalDeviceProperties(engine->physicalDevice, &engine->deviceProperties);
    if (res != VK_SUCCESS) {
        printf ("vkGetPhysicalDeviceProperties returned error %d.\n", res);
        return -1;
    }

    LOGI("vkGetPhysicalDeviceProperties");

    setupUniforms(engine);

    //Now use the descriptor layout to create a pipeline layout
    VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo;
    pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pPipelineLayoutCreateInfo.flags = 0;
    pPipelineLayoutCreateInfo.pNext = NULL;
    pPipelineLayoutCreateInfo.pushConstantRangeCount = 0;
    pPipelineLayoutCreateInfo.pPushConstantRanges = NULL;
    pPipelineLayoutCreateInfo.setLayoutCount = 2;
    pPipelineLayoutCreateInfo.pSetLayouts = engine->descriptorSetLayouts;

    res = vkCreatePipelineLayout(engine->vkDevice, &pPipelineLayoutCreateInfo, NULL, &engine->pipelineLayout);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreatePipelineLayout returned error.\n");
        return -1;
    }

    pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pPipelineLayoutCreateInfo.pNext = NULL;
    pPipelineLayoutCreateInfo.pushConstantRangeCount = 0;
    pPipelineLayoutCreateInfo.pPushConstantRanges = NULL;
    pPipelineLayoutCreateInfo.setLayoutCount = 3;
    pPipelineLayoutCreateInfo.pSetLayouts = engine->descriptorSetLayouts;

    res = vkCreatePipelineLayout(engine->vkDevice, &pPipelineLayoutCreateInfo, NULL, &engine->peelPipelineLayout);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreatePipelineLayout returned error.\n");
        return -1;
    }

    pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pPipelineLayoutCreateInfo.pNext = NULL;
    pPipelineLayoutCreateInfo.pushConstantRangeCount = 0;
    pPipelineLayoutCreateInfo.pPushConstantRanges = NULL;
    pPipelineLayoutCreateInfo.setLayoutCount = 6;
    pPipelineLayoutCreateInfo.pSetLayouts = engine->descriptorSetLayouts;

    res = vkCreatePipelineLayout(engine->vkDevice, &pPipelineLayoutCreateInfo, NULL, &engine->blendPipelineLayout);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreatePipelineLayout returned error.\n");
        return -1;
    }

    LOGI("Pipeline layout created");

    //load shaders

    VkShaderModuleCreateInfo moduleCreateInfo;
    moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCreateInfo.pNext = NULL;
    moduleCreateInfo.flags = 0;
    bool ok;
    {
        size_t vertexShaderSize=0;
        char *vertexShader = loadAsset("shaders/trad.vert.spv", engine, ok, vertexShaderSize);
        size_t fragmentShaderSize=0;
        char *fragmentShader = loadAsset("shaders/trad.frag.spv", engine, ok, fragmentShaderSize);
        if (vertexShaderSize==0 || fragmentShaderSize==0){
            LOGE ("Colud not load shader file.\n");
            return -1;
        }


        moduleCreateInfo.codeSize = vertexShaderSize;
        moduleCreateInfo.pCode = (uint32_t*)vertexShader; //This may not work with big-endian systems.
        res = vkCreateShaderModule(engine->vkDevice, &moduleCreateInfo, NULL, &engine->shdermodules[0]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateShaderModule returned error %d.\n", res);
            return -1;
        }

        moduleCreateInfo.codeSize = fragmentShaderSize;
        moduleCreateInfo.pCode = (uint32_t*)fragmentShader;
        res = vkCreateShaderModule(engine->vkDevice, &moduleCreateInfo, NULL, &engine->shdermodules[1]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateShaderModule returned error %d.\n", res);
            return -1;
        }
    }
    {
        size_t vertexShaderSize=0;
        char *vertexShader = loadAsset("shaders/peel.vert.spv", engine, ok, vertexShaderSize);
        size_t fragmentShaderSize=0;
        char *fragmentShader = loadAsset("shaders/peel.frag.spv", engine, ok, fragmentShaderSize);
        if (vertexShaderSize==0 || fragmentShaderSize==0){
            LOGE ("Colud not load shader file.\n");
            return -1;
        }

        moduleCreateInfo.codeSize = vertexShaderSize;
        moduleCreateInfo.pCode = (uint32_t*)vertexShader; //This may not work with big-endian systems.
        res = vkCreateShaderModule(engine->vkDevice, &moduleCreateInfo, NULL, &engine->shdermodules[2]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateShaderModule returned error %d.\n", res);
            return -1;
        }

        moduleCreateInfo.codeSize = fragmentShaderSize;
        moduleCreateInfo.pCode = (uint32_t*)fragmentShader;
        res = vkCreateShaderModule(engine->vkDevice, &moduleCreateInfo, NULL, &engine->shdermodules[3]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateShaderModule returned error %d.\n", res);
            return -1;
        }
    }
    {
        size_t vertexShaderSize=0;
        char *vertexShader = loadAsset("shaders/blend.vert.spv", engine, ok, vertexShaderSize);
        size_t fragmentShaderSize=0;
        char *fragmentShader = loadAsset("shaders/blend.frag.spv", engine, ok, fragmentShaderSize);
        if (vertexShaderSize==0 || fragmentShaderSize==0){
            LOGE ("Colud not load shader file.\n");
            return -1;
        }

        moduleCreateInfo.codeSize = vertexShaderSize;
        moduleCreateInfo.pCode = (uint32_t*)vertexShader; //This may not work with big-endian systems.
        res = vkCreateShaderModule(engine->vkDevice, &moduleCreateInfo, NULL, &engine->shdermodules[4]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateShaderModule returned error %d.\n", res);
            return -1;
        }

        moduleCreateInfo.codeSize = fragmentShaderSize;
        moduleCreateInfo.pCode = (uint32_t*)fragmentShader;
        res = vkCreateShaderModule(engine->vkDevice, &moduleCreateInfo, NULL, &engine->shdermodules[5]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateShaderModule returned error %d.\n", res);
            return -1;
        }
    }
    LOGI("Shaders Loaded");

    //Create the framebuffers
    engine->framebuffers=new VkFramebuffer[engine->swapchainImageCount];

    for (i = 0; i < engine->swapchainImageCount; i++) {

        VkImageView imageViewAttachments[5];

        //Attach the correct swapchain colourbuffer
        imageViewAttachments[0] = engine->swapChainViews[i];
        //We only have one depth buffer which we attach to all framebuffers
        imageViewAttachments[1] = engine->depthView[0];
        imageViewAttachments[2] = engine->peelViews[0];
        imageViewAttachments[3] = engine->depthView[1];
        imageViewAttachments[4] = engine->peelViews[1];

        VkFramebufferCreateInfo fb_info;
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.pNext = NULL;
        fb_info.renderPass = engine->renderPass;
        fb_info.attachmentCount = 5;
        fb_info.pAttachments = imageViewAttachments;
        fb_info.width = swapChainExtent.width;
        fb_info.height = swapChainExtent.height;
        fb_info.layers = 1;
        fb_info.flags = 0;

        res = vkCreateFramebuffer(engine->vkDevice, &fb_info, NULL, &engine->framebuffers[i]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateFramebuffer returned error %d.\n", res);
            return -1;
        }
    }

    LOGI("%d framebuffers created", engine->swapchainImageCount);

    //Create Vertex buffers:
    VkBufferCreateInfo vertexBufferCreateInfo;
    vertexBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertexBufferCreateInfo.pNext = NULL;
    vertexBufferCreateInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vertexBufferCreateInfo.size = sizeof(vertexDataCube)+sizeof(vetrexDataPyramid);
    vertexBufferCreateInfo.queueFamilyIndexCount = 0;
    vertexBufferCreateInfo.pQueueFamilyIndices = NULL;
    vertexBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vertexBufferCreateInfo.flags = 0;

    res = vkCreateBuffer(engine->vkDevice, &vertexBufferCreateInfo, NULL, &engine->vertexBuffer);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateBuffer returned error %d.\n", res);
        return -1;
    }

    found = 0;
    vkGetBufferMemoryRequirements(engine->vkDevice, engine->vertexBuffer, &memoryRequirements);
    typeBits = memoryRequirements.memoryTypeBits;
    requirements_mask = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (typeIndex = 0; typeIndex < engine->physicalDeviceMemoryProperties.memoryTypeCount; typeIndex++) {
        if ((typeBits & 1) == 1)//Check last bit;
        {
            if ((engine->physicalDeviceMemoryProperties.memoryTypes[typeIndex].propertyFlags & requirements_mask) == requirements_mask)
            {
                found=1;
                break;
            }
        }
        typeBits >>= 1;
    }

    if (!found)
    {
        LOGE ("Did not find a suitible memory type.\n");
        return -1;
    }else
        LOGI ("Using memory type %d.\n", typeIndex);

    memAllocInfo.pNext = NULL;
    memAllocInfo.allocationSize = memoryRequirements.size;
    memAllocInfo.memoryTypeIndex = typeIndex;

    VkDeviceMemory vertexMemory;
    res = vkAllocateMemory(engine->vkDevice, &memAllocInfo, NULL, &vertexMemory);
    if (res != VK_SUCCESS) {
        LOGE ("vkAllocateMemory returned error %d.\n", res);
        return -1;
    }

    uint8_t *vertexMappedMemory;
    res = vkMapMemory(engine->vkDevice, vertexMemory, 0, memoryRequirements.size, 0, (void **)&vertexMappedMemory);
    if (res != VK_SUCCESS) {
        LOGE ("vkMapMemory returned error %d.\n", res);
        return -1;
    }

    memcpy(vertexMappedMemory, vertexDataCube, sizeof(vertexDataCube));
    memcpy(vertexMappedMemory+sizeof(vertexDataCube), vetrexDataPyramid, sizeof(vetrexDataPyramid));

    vkUnmapMemory(engine->vkDevice, vertexMemory);

    res = vkBindBufferMemory(engine->vkDevice, engine->vertexBuffer, vertexMemory, 0);
    if (res != VK_SUCCESS) {
        LOGE ("vkBindBufferMemory returned error %d.\n", res);
        return -1;
    }
    engine->vertexInputBindingDescription.binding = 0;
    engine->vertexInputBindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    engine->vertexInputBindingDescription.stride = sizeof(vertexDataCube[0]);

    engine->vertexInputAttributeDescription.binding = 0;
    engine->vertexInputAttributeDescription.location = 0;
    engine->vertexInputAttributeDescription.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    engine->vertexInputAttributeDescription.offset = 0;
//    engine->vertexInputAttributeDescription[1].binding = 0;
//    engine->vertexInputAttributeDescription[1].location = 1;
//    engine->vertexInputAttributeDescription[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
//    engine->vertexInputAttributeDescription[1].offset = 16;

    setupTraditionalBlendPipeline(engine);
    setupPeelPipeline(engine);
    setupBlendPipeline(engine);

    VkSemaphoreCreateInfo presentCompleteSemaphoreCreateInfo;
    presentCompleteSemaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    presentCompleteSemaphoreCreateInfo.pNext = NULL;
    presentCompleteSemaphoreCreateInfo.flags = 0;

    res = vkCreateSemaphore(engine->vkDevice, &presentCompleteSemaphoreCreateInfo, NULL, &engine->presentCompleteSemaphore);
    if (res != VK_SUCCESS) {
        printf ("vkCreateSemaphore returned error.\n");
        return -1;
    }

    createSecondaryBuffers(engine);

    engine->vulkanSetupOK=true;

#ifdef __ANDROID__
#ifdef FORCE_VALIDATION
    LOGI("Restoring working directory");
    chdir(oldcwd);

    if (getcwd(cwd, sizeof(cwd)) != NULL)
        LOGI("Current working dir: %s\n", cwd);
#endif
#endif

    LOGI ("Vulkan setup complete");

    return 0;
}

int setupTraditionalBlendPipeline(struct engine* engine)
{

    LOGI("Setting up trad blend pipeline");

    VkRect2D scissor;
    scissor.extent.width = engine->width / 2;
    scissor.extent.height = engine->height;
    scissor.offset.x = 0;
    scissor.offset.y = 0;

    VkViewport viewport;
    viewport.height = (float)engine->height;
    viewport.width = (float)engine->width;
    viewport.minDepth = (float)0.0f;
    viewport.maxDepth = (float)1.0f;
    viewport.x = 0;
    viewport.y = 0;

    //Create a pipeline object
    VkDynamicState dynamicStateEnables[VK_DYNAMIC_STATE_RANGE_SIZE];
    VkPipelineDynamicStateCreateInfo dynamicState;
    //No dynamic state:
    memset(dynamicStateEnables, 0, sizeof dynamicStateEnables);
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.flags = 0;
    dynamicState.pNext = NULL;
    dynamicState.pDynamicStates = dynamicStateEnables;
    dynamicState.dynamicStateCount = 0;

    VkPipelineVertexInputStateCreateInfo vi;
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.pNext = NULL;
    vi.flags = 0;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &engine->vertexInputBindingDescription;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &engine->vertexInputAttributeDescription;

    VkPipelineInputAssemblyStateCreateInfo ia;
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.pNext = NULL;
    ia.flags = 0;
    ia.primitiveRestartEnable = VK_FALSE;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineRasterizationStateCreateInfo rs;
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.pNext = NULL;
    rs.flags = 0;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.depthClampEnable = VK_TRUE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1;

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.flags = 0;
    cb.pNext = NULL;
    VkPipelineColorBlendAttachmentState att_state[1];
    att_state[0].colorWriteMask = 0xf;
    att_state[0].blendEnable = VK_TRUE;
    att_state[0].alphaBlendOp = VK_BLEND_OP_ADD;
    att_state[0].colorBlendOp = VK_BLEND_OP_ADD;
    att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cb.attachmentCount = 1;
    cb.pAttachments = att_state;
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_NO_OP;
    cb.blendConstants[0] = 1.0f;
    cb.blendConstants[1] = 1.0f;
    cb.blendConstants[2] = 1.0f;
    cb.blendConstants[3] = 1.0f;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.pNext = NULL;
    vp.flags = 0;
    vp.viewportCount = 1;
//    dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
//    dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
    vp.pScissors = &scissor;

    VkPipelineDepthStencilStateCreateInfo ds;
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.pNext = NULL;
    ds.flags = 0;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;
    ds.back.failOp = VK_STENCIL_OP_KEEP;
    ds.back.passOp = VK_STENCIL_OP_KEEP;
    ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
    ds.back.compareMask = 0;
    ds.back.reference = 0;
    ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.back.writeMask = 0;
    ds.minDepthBounds = 0;
    ds.maxDepthBounds = 0;
    ds.stencilTestEnable = VK_FALSE;
    ds.front = ds.back;

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.pNext = NULL;
    ms.flags = 0;
    ms.pSampleMask = NULL;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;
    ms.alphaToCoverageEnable = VK_FALSE;
    ms.alphaToOneEnable = VK_FALSE;
    ms.minSampleShading = 0.0;

    VkPipelineShaderStageCreateInfo shaderStages[2];
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].pNext = NULL;
    shaderStages[0].pSpecializationInfo = NULL;
    shaderStages[0].flags = 0;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].pName = "main";
    shaderStages[0].module = engine->shdermodules[0];
    shaderStages[1].sType =  VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].pNext = NULL;
    shaderStages[1].pSpecializationInfo = NULL;
    shaderStages[1].flags = 0;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].pName = "main";
    shaderStages[1].module = engine->shdermodules[1];

    VkGraphicsPipelineCreateInfo pipelineInfo;
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = NULL;
    pipelineInfo.layout = engine->pipelineLayout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = 0;
    pipelineInfo.flags = 0;
    pipelineInfo.pVertexInputState = &vi;
    pipelineInfo.pInputAssemblyState = &ia;
    pipelineInfo.pRasterizationState = &rs;
    pipelineInfo.pColorBlendState = &cb;
    pipelineInfo.pTessellationState = NULL;
    pipelineInfo.pMultisampleState = &ms;
    pipelineInfo.pDynamicState = &dynamicState;
    if (dynamicState.dynamicStateCount==0)
        pipelineInfo.pDynamicState=NULL;
    pipelineInfo.pViewportState = &vp;
    pipelineInfo.pDepthStencilState = &ds;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.stageCount = 2;
    pipelineInfo.renderPass = engine->renderPass;
    pipelineInfo.subpass = 0;

    VkResult res;
    res = vkCreateGraphicsPipelines(engine->vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &engine->traditionalBlendPipeline);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateGraphicsPipelines returned error %d.\n", res);
        return -1;
    }
    return 0;
}


int setupPeelPipeline(struct engine* engine) {

    LOGI("Setting up peel pipeline");

    VkViewport viewport;
    viewport.height = (float) engine->height;
    viewport.width = (float) engine->width;
    viewport.minDepth = (float) 0.0f;
    viewport.maxDepth = (float) 1.0f;
    viewport.x = 0;
    viewport.y = 0;

    //Create a pipeline object
    VkDynamicState dynamicStateEnables[VK_DYNAMIC_STATE_RANGE_SIZE];
    VkPipelineDynamicStateCreateInfo dynamicState;
    //No dynamic state:
    memset(dynamicStateEnables, 0, sizeof dynamicStateEnables);
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.flags = 0;
    dynamicState.pNext = NULL;
    dynamicState.pDynamicStates = dynamicStateEnables;
    dynamicState.dynamicStateCount = 0;

    VkPipelineVertexInputStateCreateInfo vi;
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.pNext = NULL;
    vi.flags = 0;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &engine->vertexInputBindingDescription;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &engine->vertexInputAttributeDescription;

    VkPipelineInputAssemblyStateCreateInfo ia;
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.pNext = NULL;
    ia.flags = 0;
    ia.primitiveRestartEnable = VK_FALSE;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineRasterizationStateCreateInfo rs;
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.pNext = NULL;
    rs.flags = 0;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.depthClampEnable = VK_TRUE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1;

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.flags = 0;
    cb.pNext = NULL;
    VkPipelineColorBlendAttachmentState att_state[1] = {};
    att_state[0].colorWriteMask = 0xf;
    att_state[0].blendEnable = VK_FALSE;
    cb.attachmentCount = 1;
    cb.pAttachments = att_state;
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_NO_OP;
    cb.blendConstants[0] = 1.0f;
    cb.blendConstants[1] = 1.0f;
    cb.blendConstants[2] = 1.0f;
    cb.blendConstants[3] = 1.0f;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.pNext = NULL;
    vp.flags = 0;
    vp.viewportCount = 1;
//    dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
//    vp.pScissors = &scissor;

    VkPipelineDepthStencilStateCreateInfo ds;
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.pNext = NULL;
    ds.flags = 0;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;
    ds.back.failOp = VK_STENCIL_OP_KEEP;
    ds.back.passOp = VK_STENCIL_OP_KEEP;
    ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
    ds.back.compareMask = 0;
    ds.back.reference = 0;
    ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.back.writeMask = 0;
    ds.minDepthBounds = 0;
    ds.maxDepthBounds = 1;
    ds.stencilTestEnable = VK_FALSE;
    ds.front = ds.back;

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.pNext = NULL;
    ms.flags = 0;
    ms.pSampleMask = NULL;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;
    ms.alphaToCoverageEnable = VK_FALSE;
    ms.alphaToOneEnable = VK_FALSE;
    ms.minSampleShading = 0.0;

    VkPipelineShaderStageCreateInfo peelShaderStages[2];
    peelShaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    peelShaderStages[0].pNext = NULL;
    peelShaderStages[0].pSpecializationInfo = NULL;
    peelShaderStages[0].flags = 0;
    peelShaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    peelShaderStages[0].pName = "main";
    peelShaderStages[0].module = engine->shdermodules[2];
    peelShaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    peelShaderStages[1].pNext = NULL;
    peelShaderStages[1].pSpecializationInfo = NULL;
    peelShaderStages[1].flags = 0;
    peelShaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    peelShaderStages[1].pName = "main";
    peelShaderStages[1].module = engine->shdermodules[3];

    VkPipelineShaderStageCreateInfo firstPeelShaderStages[2];
    firstPeelShaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    firstPeelShaderStages[0].pNext = NULL;
    firstPeelShaderStages[0].pSpecializationInfo = NULL;
    firstPeelShaderStages[0].flags = 0;
    firstPeelShaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    firstPeelShaderStages[0].pName = "main";
    firstPeelShaderStages[0].module = engine->shdermodules[2];
    firstPeelShaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    firstPeelShaderStages[1].pNext = NULL;
    firstPeelShaderStages[1].pSpecializationInfo = NULL;
    firstPeelShaderStages[1].flags = 0;
    firstPeelShaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    firstPeelShaderStages[1].pName = "main";
    firstPeelShaderStages[1].module = engine->shdermodules[1];

    VkGraphicsPipelineCreateInfo pipelineInfo;
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = NULL;
    pipelineInfo.layout = engine->peelPipelineLayout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = 0;
    pipelineInfo.flags = 0;
    pipelineInfo.pVertexInputState = &vi;
    pipelineInfo.pInputAssemblyState = &ia;
    pipelineInfo.pRasterizationState = &rs;
    pipelineInfo.pColorBlendState = &cb;
    pipelineInfo.pTessellationState = NULL;
    pipelineInfo.pMultisampleState = &ms;
    pipelineInfo.pDynamicState = &dynamicState;
    if (dynamicState.dynamicStateCount==0)
        pipelineInfo.pDynamicState=NULL;
    pipelineInfo.pViewportState = &vp;
    pipelineInfo.pDepthStencilState = &ds;
    pipelineInfo.pStages = peelShaderStages;
    pipelineInfo.stageCount = 2;
    pipelineInfo.renderPass = engine->renderPass;
    pipelineInfo.subpass = 3;

    LOGI("Creating peel pipeline");
    VkResult res;
    res = vkCreateGraphicsPipelines(engine->vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, NULL,
                                    &engine->peelPipeline);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines returned error %d.\n", res);
        return -1;
    }

    LOGI("Creating first peel pipeline");
    pipelineInfo.layout = engine->pipelineLayout;
    pipelineInfo.pStages = firstPeelShaderStages;
    pipelineInfo.subpass = 1;

    res = vkCreateGraphicsPipelines(engine->vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, NULL,
                                    &engine->firstPeelPipeline);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines returned error %d.\n", res);
        return -1;
    }

    return 0;
}

int setupBlendPipeline(struct engine* engine) {

    LOGI("Setting up blend pipeline");
    VkViewport viewport;
    viewport.height = (float) engine->height;
    viewport.width = (float) engine->width;
    viewport.minDepth = (float) 0.0f;
    viewport.maxDepth = (float) 1.0f;
    viewport.x = 0;
    viewport.y = 0;

    //Create a pipeline object
    VkDynamicState dynamicStateEnables[VK_DYNAMIC_STATE_RANGE_SIZE];
    VkPipelineDynamicStateCreateInfo dynamicState;
    //No dynamic state:
    memset(dynamicStateEnables, 0, sizeof dynamicStateEnables);
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.flags = 0;
    dynamicState.pNext = NULL;
    dynamicState.pDynamicStates = dynamicStateEnables;
    dynamicState.dynamicStateCount = 0;

    VkPipelineVertexInputStateCreateInfo vi;
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.pNext = NULL;
    vi.flags = 0;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &engine->vertexInputBindingDescription;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &engine->vertexInputAttributeDescription;

    VkPipelineInputAssemblyStateCreateInfo ia;
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.pNext = NULL;
    ia.flags = 0;
    ia.primitiveRestartEnable = VK_FALSE;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineRasterizationStateCreateInfo rs;
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.pNext = NULL;
    rs.flags = 0;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.depthClampEnable = VK_TRUE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1;

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.flags = 0;
    cb.pNext = NULL;
    VkPipelineColorBlendAttachmentState att_state[1];
    att_state[0].colorWriteMask = 0xf;
    att_state[0].blendEnable = VK_TRUE;
    att_state[0].alphaBlendOp = VK_BLEND_OP_ADD;
    att_state[0].colorBlendOp = VK_BLEND_OP_ADD;
    att_state[0].srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
    att_state[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    att_state[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att_state[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.attachmentCount = 1;
    cb.pAttachments = att_state;
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_NO_OP;
    cb.blendConstants[0] = 1.0f;
    cb.blendConstants[1] = 1.0f;
    cb.blendConstants[2] = 1.0f;
    cb.blendConstants[3] = 1.0f;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.pNext = NULL;
    vp.flags = 0;
    vp.viewportCount = 1;
//    dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    dynamicStateEnables[dynamicState.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
//    vp.pScissors = &scissor;

    VkPipelineDepthStencilStateCreateInfo ds;
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.pNext = NULL;
    ds.flags = 0;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;
    ds.back.failOp = VK_STENCIL_OP_KEEP;
    ds.back.passOp = VK_STENCIL_OP_KEEP;
    ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
    ds.back.compareMask = 0;
    ds.back.reference = 0;
    ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.back.writeMask = 0;
    ds.minDepthBounds = 0;
    ds.maxDepthBounds = 0;
    ds.stencilTestEnable = VK_FALSE;
    ds.front = ds.back;

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.pNext = NULL;
    ms.flags = 0;
    ms.pSampleMask = NULL;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;
    ms.alphaToCoverageEnable = VK_FALSE;
    ms.alphaToOneEnable = VK_FALSE;
    ms.minSampleShading = 0.0;

    VkPipelineShaderStageCreateInfo shaderStages[2];
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].pNext = NULL;
    shaderStages[0].pSpecializationInfo = NULL;
    shaderStages[0].flags = 0;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].pName = "main";
    shaderStages[0].module = engine->shdermodules[4];
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].pNext = NULL;
    shaderStages[1].pSpecializationInfo = NULL;
    shaderStages[1].flags = 0;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].pName = "main";
    shaderStages[1].module = engine->shdermodules[5];

    VkGraphicsPipelineCreateInfo pipelineInfo;
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = NULL;
    pipelineInfo.layout = engine->blendPipelineLayout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = 0;
    pipelineInfo.flags = 0;
    pipelineInfo.pVertexInputState = &vi;
    pipelineInfo.pInputAssemblyState = &ia;
    pipelineInfo.pRasterizationState = &rs;
    pipelineInfo.pColorBlendState = &cb;
    pipelineInfo.pTessellationState = NULL;
    pipelineInfo.pMultisampleState = &ms;
    pipelineInfo.pDynamicState = &dynamicState;
    if (dynamicState.dynamicStateCount==0)
        pipelineInfo.pDynamicState=NULL;
    pipelineInfo.pViewportState = &vp;
    pipelineInfo.pDepthStencilState = &ds;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.stageCount = 2;
    pipelineInfo.renderPass = engine->renderPass;
    pipelineInfo.subpass = 2;

    VkResult res;
    res = vkCreateGraphicsPipelines(engine->vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, NULL,
                                    &engine->blendPipeline);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines returned error %d.\n", res);
        return -1;
    }
    return 0;
}

int setupUniforms(struct engine* engine)
{
    VkResult res;

    //Create a descriptor pool
    VkDescriptorPoolSize typeCounts[2];
    typeCounts[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    typeCounts[0].descriptorCount = MAX_BOXES+3;
    typeCounts[1].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    typeCounts[1].descriptorCount = 4;

    VkDescriptorPoolCreateInfo descriptorPoolInfo;
    descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.flags = 0;
    descriptorPoolInfo.pNext = NULL;
    descriptorPoolInfo.maxSets = MAX_BOXES+7;
    descriptorPoolInfo.poolSizeCount = 2;
    descriptorPoolInfo.pPoolSizes = typeCounts;

    VkDescriptorPool descriptorPool;
    res = vkCreateDescriptorPool(engine->vkDevice, &descriptorPoolInfo, NULL, &descriptorPool);
    if (res != VK_SUCCESS) {
        printf ("vkCreateDescriptorPool returned error %d.\n", res);
        return -1;
    }

    printf ("minUniformBufferOffsetAlignment %d.\n", (int)engine->deviceProperties.limits.minUniformBufferOffsetAlignment);
    engine->modelBufferValsOffset = sizeof(float)*(16+4);
    if (engine->modelBufferValsOffset < engine->deviceProperties.limits.minUniformBufferOffsetAlignment)
        engine->modelBufferValsOffset = engine->deviceProperties.limits.minUniformBufferOffsetAlignment;
    printf ("modelBufferValsOffset %d.\n", engine->modelBufferValsOffset);

    VkBufferCreateInfo uniformBufferCreateInfo;
    uniformBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    uniformBufferCreateInfo.pNext = NULL;
    uniformBufferCreateInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    uniformBufferCreateInfo.size = engine->modelBufferValsOffset*(MAX_BOXES+3); //Enough to store MAX_BOXES+3 matricies.
    uniformBufferCreateInfo.queueFamilyIndexCount = 0;
    uniformBufferCreateInfo.pQueueFamilyIndices = NULL;
    uniformBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    uniformBufferCreateInfo.flags = 0;

    VkBuffer uniformBuffer;
    res = vkCreateBuffer(engine->vkDevice, &uniformBufferCreateInfo, NULL, &uniformBuffer);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateBuffer returned error %d.\n", res);
        return -1;
    }

    VkMemoryRequirements memoryRequirements;
    vkGetBufferMemoryRequirements(engine->vkDevice, uniformBuffer, &memoryRequirements);
    uint8_t found = 0;
    uint32_t typeBits = memoryRequirements.memoryTypeBits;
    LOGI("Uniform memory types %d", typeBits);
    VkFlags requirements_mask = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t typeIndex;
    for (typeIndex = 0; typeIndex < engine->physicalDeviceMemoryProperties.memoryTypeCount; typeIndex++) {
        if ((typeBits & 1) == 1)//Check last bit;
        {
            if ((engine->physicalDeviceMemoryProperties.memoryTypes[typeIndex].propertyFlags & requirements_mask) == requirements_mask)
            {
                found=1;
                break;
            }
        }
        typeBits >>= 1;
    }

    if (!found)
    {
        LOGE ("Did not find a suitable memory type.\n");
        return -1;
    }else
        LOGI ("Using memory type %d.\n", typeIndex);

    VkMemoryAllocateInfo memAllocInfo;
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.pNext = NULL;
    memAllocInfo.allocationSize = memoryRequirements.size;
    memAllocInfo.memoryTypeIndex = typeIndex;
    //
    VkDeviceMemory uniformMemory;
    res = vkAllocateMemory(engine->vkDevice, &memAllocInfo, NULL, &uniformMemory);
    if (res != VK_SUCCESS) {
        LOGE ("vkCreateBuffer returned error %d.\n", res);
        return -1;
    }

    res = vkMapMemory(engine->vkDevice, uniformMemory, 0, memoryRequirements.size, 0, (void **)&engine->uniformMappedMemory);
    if (res != VK_SUCCESS) {
        LOGE ("vkMapMemory returned error %d.\n", res);
        return -1;
    }

    res = vkBindBufferMemory(engine->vkDevice, uniformBuffer, uniformMemory, 0);
    if (res != VK_SUCCESS) {
        LOGE ("vkBindBufferMemory returned error %d.\n", res);
        return -1;
    }

    updateColours(engine);

    engine->descriptorSetLayouts = new VkDescriptorSetLayout[6];

    for (int i = 0; i <6; i++) {
        VkDescriptorSetLayoutBinding layout_bindings[1];
        layout_bindings[0].binding = 0;
        layout_bindings[0].descriptorType = (i<2) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        layout_bindings[0].descriptorCount = 1;
        layout_bindings[0].stageFlags = (i<2) ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        layout_bindings[0].pImmutableSamplers = NULL;

        //Next take layout bindings and use them to create a descriptor set layout
        VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
        descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorSetLayoutCreateInfo.flags = 0;
        descriptorSetLayoutCreateInfo.pNext = NULL;
        descriptorSetLayoutCreateInfo.bindingCount = 1;
        descriptorSetLayoutCreateInfo.pBindings = layout_bindings;

        res = vkCreateDescriptorSetLayout(engine->vkDevice, &descriptorSetLayoutCreateInfo, NULL,
                                          &engine->descriptorSetLayouts[i]);
        if (res != VK_SUCCESS) {
            LOGE ("vkCreateDescriptorSetLayout returned error.\n");
            return -1;
        }
    }

    //Create the descriptor sets

    VkDescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.pNext = NULL;
    descriptorSetAllocateInfo.descriptorPool = descriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = &engine->descriptorSetLayouts[0];

    res = vkAllocateDescriptorSets(engine->vkDevice, &descriptorSetAllocateInfo, &engine->sceneDescriptorSet);
    if (res != VK_SUCCESS) {
        printf ("vkAllocateDescriptorSets returned error %d.\n", res);
        return -1;
    }

    res = vkAllocateDescriptorSets(engine->vkDevice, &descriptorSetAllocateInfo, &engine->identitySceneDescriptorSet);
    if (res != VK_SUCCESS) {
        printf ("vkAllocateDescriptorSets returned error %d.\n", res);
        return -1;
    }

    engine->modelDescriptorSets = new VkDescriptorSet[MAX_BOXES];
    VkDescriptorSetLayout sceneLayouts[MAX_BOXES];
    for (int i=0; i<MAX_BOXES; i++)
        sceneLayouts[i]=engine->descriptorSetLayouts[1];

    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.pNext = NULL;
    descriptorSetAllocateInfo.descriptorPool = descriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = MAX_BOXES;
    descriptorSetAllocateInfo.pSetLayouts = sceneLayouts;

    res = vkAllocateDescriptorSets(engine->vkDevice, &descriptorSetAllocateInfo, engine->modelDescriptorSets);
    if (res != VK_SUCCESS) {
        printf ("vkAllocateDescriptorSets returned error %d.\n", res);
        return -1;
    }

    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.pNext = NULL;
    descriptorSetAllocateInfo.descriptorPool = descriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = engine->descriptorSetLayouts;

    res = vkAllocateDescriptorSets(engine->vkDevice, &descriptorSetAllocateInfo, &engine->identityModelDescriptorSet);
    if (res != VK_SUCCESS) {
        printf ("vkAllocateDescriptorSets returned error %d.\n", res);
        return -1;
    }

    //The input attachments:
    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.pNext = NULL;
    descriptorSetAllocateInfo.descriptorPool = descriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = &engine->descriptorSetLayouts[2];
    res = vkAllocateDescriptorSets(engine->vkDevice, &descriptorSetAllocateInfo, &engine->colourInputAttachmentDescriptorSets[0]);
    if (res != VK_SUCCESS) {
        printf ("vkAllocateDescriptorSets returned error %d.\n", res);
        return -1;
    }

    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.pNext = NULL;
    descriptorSetAllocateInfo.descriptorPool = descriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = &engine->descriptorSetLayouts[2];
    res = vkAllocateDescriptorSets(engine->vkDevice, &descriptorSetAllocateInfo, &engine->colourInputAttachmentDescriptorSets[1]);
    if (res != VK_SUCCESS) {
        printf ("vkAllocateDescriptorSets returned error %d.\n", res);
        return -1;
    }

    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.pNext = NULL;
    descriptorSetAllocateInfo.descriptorPool = descriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = &engine->descriptorSetLayouts[2];
    res = vkAllocateDescriptorSets(engine->vkDevice, &descriptorSetAllocateInfo, &engine->depthInputAttachmentDescriptorSets[0]);
    if (res != VK_SUCCESS) {
        printf ("vkAllocateDescriptorSets returned error %d.\n", res);
        return -1;
    }

    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.pNext = NULL;
    descriptorSetAllocateInfo.descriptorPool = descriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = &engine->descriptorSetLayouts[2];
    res = vkAllocateDescriptorSets(engine->vkDevice, &descriptorSetAllocateInfo, &engine->depthInputAttachmentDescriptorSets[1]);
    if (res != VK_SUCCESS) {
        printf ("vkAllocateDescriptorSets returned error %d.\n", res);
        return -1;
    }


    VkDescriptorBufferInfo uniformBufferInfo[MAX_BOXES+3];
    VkWriteDescriptorSet writes[MAX_BOXES+7];
    for (int i = 0; i<MAX_BOXES+3; i++) {
        uniformBufferInfo[i].buffer = uniformBuffer;
        uniformBufferInfo[i].offset = engine->modelBufferValsOffset*i;
        uniformBufferInfo[i].range = sizeof(float) * 16;
    }
    for (int i = 0; i<MAX_BOXES; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].pNext = NULL;
        writes[i].dstSet = engine->modelDescriptorSets[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &uniformBufferInfo[i];
        writes[i].dstArrayElement = 0;
        writes[i].dstBinding = 0;
    }

    //Scene data
    writes[MAX_BOXES].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[MAX_BOXES].pNext = NULL;
    writes[MAX_BOXES].dstSet = engine->sceneDescriptorSet;
    writes[MAX_BOXES].descriptorCount = 1;
    writes[MAX_BOXES].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[MAX_BOXES].pBufferInfo = &uniformBufferInfo[MAX_BOXES];
    writes[MAX_BOXES].dstArrayElement = 0;
    writes[MAX_BOXES].dstBinding = 0;

    //Identity model matrix
    writes[MAX_BOXES+1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[MAX_BOXES+1].pNext = NULL;
    writes[MAX_BOXES+1].dstSet = engine->identityModelDescriptorSet;
    writes[MAX_BOXES+1].descriptorCount = 1;
    writes[MAX_BOXES+1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[MAX_BOXES+1].pBufferInfo = &uniformBufferInfo[MAX_BOXES+1];
    writes[MAX_BOXES+1].dstArrayElement = 0;
    writes[MAX_BOXES+1].dstBinding = 0;

    //Identity scene matrix
    writes[MAX_BOXES+2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[MAX_BOXES+2].pNext = NULL;
    writes[MAX_BOXES+2].dstSet = engine->identitySceneDescriptorSet;
    writes[MAX_BOXES+2].descriptorCount = 1;
    writes[MAX_BOXES+2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[MAX_BOXES+2].pBufferInfo = &uniformBufferInfo[MAX_BOXES+2];
    writes[MAX_BOXES+2].dstArrayElement = 0;
    writes[MAX_BOXES+2].dstBinding = 0;

    //The input attachment:
    VkDescriptorImageInfo peelUniformImageInfo[2];
    peelUniformImageInfo[0].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
    peelUniformImageInfo[0].imageView=engine->peelViews[0];
    peelUniformImageInfo[0].sampler=NULL;
    writes[MAX_BOXES+3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[MAX_BOXES+3].pNext = NULL;
    writes[MAX_BOXES+3].dstSet = engine->colourInputAttachmentDescriptorSets[0];
    writes[MAX_BOXES+3].descriptorCount = 1;
    writes[MAX_BOXES+3].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    writes[MAX_BOXES+3].pImageInfo=&peelUniformImageInfo[0];
    writes[MAX_BOXES+3].dstArrayElement = 0;
    writes[MAX_BOXES+3].dstBinding = 0;

    peelUniformImageInfo[1].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
    peelUniformImageInfo[1].imageView=engine->peelViews[1];
    peelUniformImageInfo[1].sampler=NULL;
    writes[MAX_BOXES+4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[MAX_BOXES+4].pNext = NULL;
    writes[MAX_BOXES+4].dstSet = engine->colourInputAttachmentDescriptorSets[1];
    writes[MAX_BOXES+4].descriptorCount = 1;
    writes[MAX_BOXES+4].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    writes[MAX_BOXES+4].pImageInfo=&peelUniformImageInfo[1];
    writes[MAX_BOXES+4].dstArrayElement = 0;
    writes[MAX_BOXES+4].dstBinding = 0;

    VkDescriptorImageInfo depthuniformImageInfo[2];
    depthuniformImageInfo[0].imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthuniformImageInfo[0].imageView=engine->depthView[0];
    depthuniformImageInfo[0].sampler=NULL;
    writes[MAX_BOXES+5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[MAX_BOXES+5].pNext = NULL;
    writes[MAX_BOXES+5].dstSet = engine->depthInputAttachmentDescriptorSets[0];
    writes[MAX_BOXES+5].descriptorCount = 1;
    writes[MAX_BOXES+5].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    writes[MAX_BOXES+5].pImageInfo=&depthuniformImageInfo[0];
    writes[MAX_BOXES+5].dstArrayElement = 0;
    writes[MAX_BOXES+5].dstBinding = 0;

    depthuniformImageInfo[1].imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthuniformImageInfo[1].imageView=engine->depthView[1];
    depthuniformImageInfo[1].sampler=NULL;
    writes[MAX_BOXES+6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[MAX_BOXES+6].pNext = NULL;
    writes[MAX_BOXES+6].dstSet = engine->depthInputAttachmentDescriptorSets[1];
    writes[MAX_BOXES+6].descriptorCount = 1;
    writes[MAX_BOXES+6].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    writes[MAX_BOXES+6].pImageInfo=&depthuniformImageInfo[1];
    writes[MAX_BOXES+6].dstArrayElement = 0;
    writes[MAX_BOXES+6].dstBinding = 0;

    vkUpdateDescriptorSets(engine->vkDevice, MAX_BOXES+7, writes, 0, NULL);

    LOGI ("Descriptor sets updated %d.\n", res);
    return 0;
}

void createSecondaryBuffers(struct engine* engine)
{
    LOGI("Creating Secondary Buffers");
    LOGI("Creating trad blend buffers");
    engine->rebuildCommadBuffersRequired=false;
    for (int i = 0; i< engine->swapchainImageCount; i++) {
        VkResult res;
        VkCommandBufferInheritanceInfo commandBufferInheritanceInfo;
        commandBufferInheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        commandBufferInheritanceInfo.pNext = 0;
        commandBufferInheritanceInfo.renderPass = engine->renderPass;
        commandBufferInheritanceInfo.subpass = 0;
        commandBufferInheritanceInfo.framebuffer = engine->framebuffers[i];
        commandBufferInheritanceInfo.occlusionQueryEnable = 0;
        commandBufferInheritanceInfo.queryFlags = 0;
        commandBufferInheritanceInfo.pipelineStatistics = 0;

        VkCommandBufferBeginInfo commandBufferBeginInfo = {};
        commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        commandBufferBeginInfo.pNext = NULL;
        commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        commandBufferBeginInfo.pInheritanceInfo = &commandBufferInheritanceInfo;
        LOGI("Creating Secondary Buffer %d using subpass %d (%d boxes)", i, 0, engine->boxCount);
        res = vkBeginCommandBuffer(engine->secondaryCommandBuffers[i], &commandBufferBeginInfo);
        if (res != VK_SUCCESS) {
            printf("vkBeginCommandBuffer returned error.\n");
            return;
        }

        vkCmdBindPipeline(engine->secondaryCommandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          engine->traditionalBlendPipeline);

        vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[i],
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                engine->pipelineLayout, 1, 1,
                                &engine->sceneDescriptorSet, 0, NULL);
        VkDeviceSize offsets[1];
        //Draw Cubes:
        offsets[0] = 0;
        vkCmdBindVertexBuffers(engine->secondaryCommandBuffers[i], 0, 1, &engine->vertexBuffer,
                               offsets);
        for (int object = 0; object < engine->boxCount/2; object++) {
            vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[i],
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    engine->pipelineLayout, 0, 1,
                                    &engine->modelDescriptorSets[object], 0, NULL);

            vkCmdDraw(engine->secondaryCommandBuffers[i], 12 * 3, 1, 0, 0);
        }
        //Draw Pyramids:
        offsets[0] = sizeof(vertexDataCube);
        vkCmdBindVertexBuffers(engine->secondaryCommandBuffers[i], 0, 1, &engine->vertexBuffer,
                               offsets);
        for (int object = MAX_BOXES/2; object < MAX_BOXES/2+engine->boxCount/2; object++) {
            vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[i],
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    engine->pipelineLayout, 0, 1,
                                    &engine->modelDescriptorSets[object], 0, NULL);

            vkCmdDraw(engine->secondaryCommandBuffers[i], 6 * 3, 1, 0, 0);
        }
        res = vkEndCommandBuffer(engine->secondaryCommandBuffers[i]);
        if (res != VK_SUCCESS) {
            printf("vkBeginCommandBuffer returned error.\n");
            return;
        }
    }
    LOGI("Creating peel stage buffers");
    for (int layer = 0; layer < MAX_LAYERS; layer++) {
        for (int i = 0; i < engine->swapchainImageCount; i++) {
            int cmdBuffIndex = engine->swapchainImageCount + layer * engine->swapchainImageCount * 2 + i;
            VkResult res;
            VkCommandBufferInheritanceInfo commandBufferInheritanceInfo;
            commandBufferInheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            commandBufferInheritanceInfo.pNext = 0;
            commandBufferInheritanceInfo.renderPass = engine->renderPass;
            commandBufferInheritanceInfo.subpass = layer*2+1;
            commandBufferInheritanceInfo.framebuffer = engine->framebuffers[i];
            commandBufferInheritanceInfo.occlusionQueryEnable = 0;
            commandBufferInheritanceInfo.queryFlags = 0;
            commandBufferInheritanceInfo.pipelineStatistics = 0;

            VkCommandBufferBeginInfo commandBufferBeginInfo = {};
            commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            commandBufferBeginInfo.pNext = NULL;
            commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            commandBufferBeginInfo.pInheritanceInfo = &commandBufferInheritanceInfo;
            LOGI("Creating Secondary Buffer %d using subpass %d (layer %d, swapchainImage %d)", cmdBuffIndex, commandBufferInheritanceInfo.subpass, layer, i);
            res = vkBeginCommandBuffer(engine->secondaryCommandBuffers[cmdBuffIndex],
                                       &commandBufferBeginInfo);
            if (res != VK_SUCCESS) {
                printf("vkBeginCommandBuffer returned error.\n");
                return;
            }

            //Clear the peel colour buffer
            {
                VkClearAttachment clear[2];
                clear[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                clear[0].clearValue.color.float32[0] = 0.0f;
                clear[0].clearValue.color.float32[1] = 0.0f;
                clear[0].clearValue.color.float32[2] = 0.0f;
                clear[0].clearValue.color.float32[3] = 0.0f;
                clear[0].colorAttachment=0;
                clear[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                clear[1].clearValue.depthStencil.depth = 1.0f;
                clear[1].clearValue.depthStencil.stencil = 0;
                VkClearRect clearRect;
                clearRect.baseArrayLayer=0;
                clearRect.layerCount=1;
                clearRect.rect.extent.height=engine->height;
                clearRect.rect.extent.width=engine->width;
                clearRect.rect.offset.x=0;
                clearRect.rect.offset.y=0;
                vkCmdClearAttachments(engine->secondaryCommandBuffers[cmdBuffIndex], 2, clear, 1, &clearRect);
            }

            vkCmdBindPipeline(engine->secondaryCommandBuffers[cmdBuffIndex],
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              (layer==0) ? engine->firstPeelPipeline : engine->peelPipeline);

            VkRect2D scissor;
            if (engine->splitscreen)
                scissor.extent.width = engine->width / 2;
            else
                scissor.extent.width = engine->width;
            scissor.extent.height = engine->height;
            if (engine->splitscreen)
                scissor.offset.x = scissor.extent.width;
            else
                scissor.offset.x = 0;
            scissor.offset.y = 0;

            vkCmdSetScissor(engine->secondaryCommandBuffers[cmdBuffIndex], 0, 1, &scissor);

            vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    (layer==0) ? engine->pipelineLayout : engine->blendPipelineLayout, 1, 1,
                                    &engine->sceneDescriptorSet, 0, NULL);
            VkDeviceSize offsets[1] = {0};
//            vkCmdBindVertexBuffers(engine->secondaryCommandBuffers[cmdBuffIndex], 0, 1,
//                                   &engine->vertexBuffer,
//                                   offsets);

            if (layer>0)
            {
                vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        engine->blendPipelineLayout, 2, 1,
                                        &engine->depthInputAttachmentDescriptorSets[!(layer%2)], 0, NULL);
            }

            //Draw Cubes:
            offsets[0] = 0;
            vkCmdBindVertexBuffers(engine->secondaryCommandBuffers[cmdBuffIndex], 0, 1, &engine->vertexBuffer,
                                   offsets);
            for (int object = 0; object < engine->boxCount/2; object++) {
                vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        engine->pipelineLayout, 0, 1,
                                        &engine->modelDescriptorSets[object], 0, NULL);

                vkCmdDraw(engine->secondaryCommandBuffers[cmdBuffIndex], 12 * 3, 1, 0, 0);
            }
            //Draw Pyramids:
            offsets[0] = sizeof(vertexDataCube);
            vkCmdBindVertexBuffers(engine->secondaryCommandBuffers[cmdBuffIndex], 0, 1, &engine->vertexBuffer,
                                   offsets);
            for (int object = MAX_BOXES/2; object < MAX_BOXES/2+engine->boxCount/2; object++) {
                vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        engine->pipelineLayout, 0, 1,
                                        &engine->modelDescriptorSets[object], 0, NULL);

                vkCmdDraw(engine->secondaryCommandBuffers[cmdBuffIndex], 6 * 3, 1, 0, 0);
            }

            //Test clearing depth buffer at end
            VkClearAttachment clear;
            clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            clear.clearValue.depthStencil.depth = 0.0f;
            clear.clearValue.depthStencil.stencil = 0;
            VkClearRect clearRect;
            clearRect.baseArrayLayer=0;
            clearRect.layerCount=1;
            clearRect.rect.extent.height=engine->height/4*2;
            clearRect.rect.extent.width=engine->width/4*2;
            clearRect.rect.offset.x=engine->height/4;
            clearRect.rect.offset.y=engine->width/4;
            //vkCmdClearAttachments(engine->secondaryCommandBuffers[cmdBuffIndex], 1, &clear, 1, &clearRect);

            res = vkEndCommandBuffer(engine->secondaryCommandBuffers[cmdBuffIndex]);
            if (res != VK_SUCCESS) {
                printf("vkBeginCommandBuffer returned error.\n");
                return;
            }
        }
    }
    LOGI("Creating blend stage buffers");
    for (int layer = 1; layer < MAX_LAYERS; layer++) {
        for (int i = 0; i < engine->swapchainImageCount; i++) {
            int cmdBuffIndex = engine->swapchainImageCount + engine->swapchainImageCount * layer * 2 + i + engine->swapchainImageCount;
            VkResult res;
            VkCommandBufferInheritanceInfo commandBufferInheritanceInfo;
            commandBufferInheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            commandBufferInheritanceInfo.pNext = 0;
            commandBufferInheritanceInfo.renderPass = engine->renderPass;
            commandBufferInheritanceInfo.subpass = layer*2+2;
            commandBufferInheritanceInfo.framebuffer = engine->framebuffers[i];
            commandBufferInheritanceInfo.occlusionQueryEnable = 0;
            commandBufferInheritanceInfo.queryFlags = 0;
            commandBufferInheritanceInfo.pipelineStatistics = 0;

            VkCommandBufferBeginInfo commandBufferBeginInfo = {};
            commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            commandBufferBeginInfo.pNext = NULL;
            commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            commandBufferBeginInfo.pInheritanceInfo = &commandBufferInheritanceInfo;

            LOGI("Creating secondaryCommandBuffer %d using subpass %d (layer %d, swapchainImage %d)", cmdBuffIndex, layer*2+2, layer, i);
            res = vkBeginCommandBuffer(engine->secondaryCommandBuffers[cmdBuffIndex],
                                       &commandBufferBeginInfo);
            if (res != VK_SUCCESS) {
                printf("vkBeginCommandBuffer returned error.\n");
                return;
            }

            vkCmdBindPipeline(engine->secondaryCommandBuffers[cmdBuffIndex],
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              engine->blendPipeline);

            VkRect2D scissor;
            if (engine->splitscreen)
                scissor.extent.width = engine->width / 2;
            else
                scissor.extent.width = engine->width;
            scissor.extent.height = engine->height;
            if (engine->splitscreen)
                scissor.offset.x = scissor.extent.width;
            else
                scissor.offset.x = 0;
            scissor.offset.y = 0;

            vkCmdSetScissor(engine->secondaryCommandBuffers[cmdBuffIndex], 0, 1, &scissor);

            vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    engine->blendPipelineLayout, 1, 1,
                                    &engine->identitySceneDescriptorSet, 0, NULL);

            VkDeviceSize offsets[1] = {0};
            vkCmdBindVertexBuffers(engine->secondaryCommandBuffers[cmdBuffIndex], 0, 1,
                                   &engine->vertexBuffer,
                                   offsets);

            vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    engine->blendPipelineLayout, 0, 1,
                                    &engine->identityModelDescriptorSet, 0, NULL);

            vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    engine->blendPipelineLayout, 2, 1,
                                    &engine->colourInputAttachmentDescriptorSets[(layer%2)], 0, NULL);


            vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    engine->blendPipelineLayout, 5, 1,
                                    &engine->colourInputAttachmentDescriptorSets[!(layer%2)], 0, NULL);


            VkDescriptorSet depthInputAttachmentDescriptorSets[2];
            depthInputAttachmentDescriptorSets[0]=engine->depthInputAttachmentDescriptorSets[(layer%2)];
            depthInputAttachmentDescriptorSets[1]=engine->depthInputAttachmentDescriptorSets[!(layer%2)];

            vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    engine->blendPipelineLayout, 3, 2,
                                    depthInputAttachmentDescriptorSets, 0, NULL);


            vkCmdDraw(engine->secondaryCommandBuffers[cmdBuffIndex], 12 * 3, 1, 0, 0);
//            for (int object = 0; object < MAX_BOXES; object++) {
//                vkCmdBindDescriptorSets(engine->secondaryCommandBuffers[cmdBuffIndex],
//                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
//                                        engine->blendPipelineLayout, 0, 1,
//                                        &engine->modelDescriptorSets[object], 0, NULL);
//
//                vkCmdDraw(engine->secondaryCommandBuffers[cmdBuffIndex], 12 * 3, 1, 0, 0);
//            }

            res = vkEndCommandBuffer(engine->secondaryCommandBuffers[cmdBuffIndex]);
            if (res != VK_SUCCESS) {
                printf("vkBeginCommandBuffer returned error.\n");
                return;
            }
        }
    }
}

void updateColours(struct engine* engine)
{
    for (int i=0; i<MAX_BOXES; i++)
    {
        float *colour = ((float*)(engine->uniformMappedMemory + engine->modelBufferValsOffset*i))+16;
        colour[0] = (float)rand() / RAND_MAX;
        colour[1] = (float)rand() / RAND_MAX;
        colour[2] = (float)rand() / RAND_MAX;
        colour[3] = 0.5;
    }
}

void updateUniforms(struct engine* engine)
{
    perspective_matrix(0.7853 /* 45deg */, (float)engine->width/(float)engine->height, 16.0f, 41.0f, (float*)(engine->uniformMappedMemory + engine->modelBufferValsOffset*MAX_BOXES));
    identity_matrix((float*)(engine->uniformMappedMemory + engine->modelBufferValsOffset*(MAX_BOXES+1)));
    identity_matrix((float*)(engine->uniformMappedMemory + engine->modelBufferValsOffset*(MAX_BOXES+2)));
    engine->simulation->write(engine->uniformMappedMemory, engine->modelBufferValsOffset);
}

/**
 * Just the current frame in the display.
 */
static void engine_draw_frame(struct engine* engine) {

    if (!engine->vulkanSetupOK) {
//        LOGI("engine_draw_frame %d", engine->frame);
//        LOGI("Vulkan not ready");
        return;
    }

//    if (engine->frame>0)
//        return;

//    sleep(1);

    VkClearValue clearValues[3];
    clearValues[0].color.float32[0] = 0.0f;
    clearValues[0].color.float32[1] = 0.0f;
    clearValues[0].color.float32[2] = 0.0f;
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil.depth = 0.0f;
    clearValues[1].depthStencil.stencil = 0;
    clearValues[2].color.float32[0] = 0.0f;
    clearValues[2].color.float32[1] = 0.0f;
    clearValues[2].color.float32[2] = 0.0f;
    clearValues[2].color.float32[3] = 1.0f;

    //The queue is idle, now is a good time to update the bound memory.
    updateUniforms(engine);

    if (engine->rebuildCommadBuffersRequired)
        createSecondaryBuffers(engine);

    uint32_t currentBuffer;
    VkResult res;

    // Get next image in the swap chain (back/front buffer)
    res = vkAcquireNextImageKHR(engine->vkDevice, engine->swapchain, UINT64_MAX,
                                engine->presentCompleteSemaphore, NULL, &currentBuffer);
    if (res != VK_SUCCESS) {
        LOGE ("vkAcquireNextImageKHR returned error.\n");
        return;
    }

//    LOGI("Using buffer %d", currentBuffer);

    //Now record the primary command buffer:
    VkRenderPassBeginInfo renderPassBeginInfo;
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.pNext = NULL;
    renderPassBeginInfo.renderPass = engine->renderPass;
    renderPassBeginInfo.framebuffer = engine->framebuffers[currentBuffer];
    renderPassBeginInfo.renderArea.offset.x = 0;
    renderPassBeginInfo.renderArea.offset.y = 0;
    renderPassBeginInfo.renderArea.extent.width = engine->width;
    renderPassBeginInfo.renderArea.extent.height = engine->height;
    renderPassBeginInfo.clearValueCount = 3;
    renderPassBeginInfo.pClearValues = clearValues;// + (i*2);

    VkCommandBufferBeginInfo commandBufferBeginInfo = {};
    commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    commandBufferBeginInfo.pNext = NULL;
    commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    commandBufferBeginInfo.pInheritanceInfo = NULL;
    res = vkBeginCommandBuffer(engine->renderCommandBuffer[0], &commandBufferBeginInfo);
    if (res != VK_SUCCESS) {
        printf("vkBeginCommandBuffer returned error.\n");
        return;
    }

    VkImageMemoryBarrier imageMemoryBarrier;
    imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imageMemoryBarrier.pNext = NULL;
    imageMemoryBarrier.image = engine->swapChainImages[currentBuffer];
    imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
    imageMemoryBarrier.subresourceRange.levelCount = 1;
    imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
    imageMemoryBarrier.subresourceRange.layerCount = 1;
    imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.srcAccessMask = 0;
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkPipelineStageFlags srcStageFlags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkPipelineStageFlags destStageFlags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    vkCmdPipelineBarrier(engine->renderCommandBuffer[0], srcStageFlags, destStageFlags, 0,
                         0, NULL, 0, NULL, 1, &imageMemoryBarrier);

    vkCmdBeginRenderPass(engine->renderCommandBuffer[0], &renderPassBeginInfo,
                         VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

    if (engine->splitscreen) {
        //Draw using traditional depth dependent transparency:
//        LOGI("Trad: Executing secondaryCommandBuffer %d", currentBuffer);
        vkCmdExecuteCommands(engine->renderCommandBuffer[0], 1,
                             &engine->secondaryCommandBuffers[currentBuffer]);
    }

    for (int layer = 0; layer < engine->layerCount; layer++) {
        int cmdBuffIndex = engine->swapchainImageCount + layer * engine->swapchainImageCount*2 + currentBuffer;
        //Peel
        vkCmdNextSubpass(engine->renderCommandBuffer[0],
                         VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
//        LOGI("Peel: Executing secondaryCommandBuffer %d", cmdBuffIndex);
        vkCmdExecuteCommands(engine->renderCommandBuffer[0], 1,
                           &engine->secondaryCommandBuffers[cmdBuffIndex]);
        //Blend
        vkCmdNextSubpass(engine->renderCommandBuffer[0],
                         VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        if (engine->displayLayer < 0 || layer==engine->displayLayer)
        {
//        LOGI("Blend: Executing secondaryCommandBuffer %d", cmdBuffIndex + engine->swapchainImageCount);
            if (layer!=0)
                vkCmdExecuteCommands(engine->renderCommandBuffer[0], 1,
                             &engine->secondaryCommandBuffers[cmdBuffIndex + engine->swapchainImageCount]);
        }
    }

    vkCmdEndRenderPass(engine->renderCommandBuffer[0]);

    VkImageMemoryBarrier prePresentBarrier;
    prePresentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prePresentBarrier.pNext = NULL;
    prePresentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    prePresentBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    prePresentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    prePresentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    prePresentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prePresentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prePresentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    prePresentBarrier.subresourceRange.baseMipLevel = 0;
    prePresentBarrier.subresourceRange.levelCount = 1;
    prePresentBarrier.subresourceRange.baseArrayLayer = 0;
    prePresentBarrier.subresourceRange.layerCount = 1;
    prePresentBarrier.image = engine->swapChainImages[currentBuffer];
    vkCmdPipelineBarrier(engine->renderCommandBuffer[0], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &prePresentBarrier);

    res = vkEndCommandBuffer(engine->renderCommandBuffer[0]);
    if (res != VK_SUCCESS) {
        LOGE ("vkEndCommandBuffer returned error %d.\n", res);
        return;
    }

    VkPipelineStageFlags pipe_stage_flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSubmitInfo submitInfo[2];
    submitInfo[0].pNext = NULL;
    submitInfo[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo[0].waitSemaphoreCount = 1; //Only the first command buffer should wait on a semaphore,
    submitInfo[0].pWaitSemaphores = &engine->presentCompleteSemaphore;
    submitInfo[0].pWaitDstStageMask = &pipe_stage_flags;
    submitInfo[0].commandBufferCount = 1;
    submitInfo[0].pCommandBuffers = engine->renderCommandBuffer;
    submitInfo[0].signalSemaphoreCount = 0;
    submitInfo[0].pSignalSemaphores = NULL;


    res = vkQueueSubmit(engine->queue, 1, submitInfo, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        LOGE ("vkQueueSubmit returned error %d.\n", res);
        return;
    }

//    LOGI ("Waiting.\n");

    res = vkQueueWaitIdle(engine->queue);
    if (res != VK_SUCCESS) {
        LOGE ("vkQueueSubmit returned error %d.\n", res);
        return;
    }

//    LOGI ("Presentng.\n");

    VkPresentInfoKHR presentInfo;
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = NULL;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &engine->swapchain;
    presentInfo.pImageIndices = &currentBuffer;
    presentInfo.pWaitSemaphores = NULL;
    presentInfo.waitSemaphoreCount = 0;
    presentInfo.pResults = NULL;
    res = vkQueuePresentKHR(engine->queue, &presentInfo);
    if (res != VK_SUCCESS) {
        LOGE ("vkQueuePresentKHR returned error %d.\n", res);
        return;
    }

//    LOGI ("Finished frame %d.\n", engine->frame);
    engine->frame++;
    if (engine->frame % 120 == 0) {
        float frameRate = (120.0f/((float)(engine->frameRateClock->getTimeMilliseconds())/1000.0f));
        LOGI("Framerate: %f", frameRate);
        engine->frameRateClock->reset();
    }
}

/**
 * Tear down the EGL context currently associated with the display.
 */
static void engine_term_display(struct engine* engine) {
    LOGI("engine_term_display");
//    vkDestroyImageView(engine->vkDevice, engine->depthView, NULL);
//    vkDestroyImage(engine->vkDevice, engine->depthImage, NULL);
//    vkFreeMemory(engine->vkDevice, engine->depthMemory, NULL);
//    if (engine->display != EGL_NO_DISPLAY) {
//        eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
//        if (engine->context != EGL_NO_CONTEXT) {
//            eglDestroyContext(engine->display, engine->context);
//        }
//        if (engine->surface != EGL_NO_SURFACE) {
//            eglDestroySurface(engine->display, engine->surface);
//        }
//        eglTerminate(engine->display);
//    }
//    engine->animating = 0;
//    engine->display = EGL_NO_DISPLAY;
//    engine->context = EGL_NO_CONTEXT;
//    engine->surface = EGL_NO_SURFACE;
}

#ifdef __ANDROID__
/**
 * Process the next input event.
 */
static int32_t engine_handle_input(struct android_app* app, AInputEvent* event) {
    struct engine* engine = (struct engine*)app->userData;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
    LOGI("engine_handle_input");
        engine->animating = 1;
        engine->state.x = AMotionEvent_getX(event, 0);
        engine->state.y = AMotionEvent_getY(event, 0);
        return 1;
    }
    else if( AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY ) {
        int32_t keycode = AKeyEvent_getKeyCode(event);
        int32_t repeatCount = AKeyEvent_getRepeatCount(event);
        int32_t action = AKeyEvent_getAction(event);
        int32_t metaState = AKeyEvent_getMetaState(event);
        int32_t devId = AInputEvent_getDeviceId(event);
//        LOGI("Key pressed %d, %s", keycode, (action == AKEY_EVENT_ACTION_DOWN) ? "Down" : "Up");
        if (keycode==AKEYCODE_DPAD_CENTER && action == AKEY_EVENT_ACTION_DOWN) {
            engine->splitscreen = !engine->splitscreen;
            engine->rebuildCommadBuffersRequired=true;
        }
        if ((keycode==AKEYCODE_DPAD_DOWN || keycode==AKEYCODE_DPAD_UP) && action == AKEY_EVENT_ACTION_DOWN) {
            if (keycode==AKEYCODE_DPAD_UP)
                engine->layerCount++;
            else
                engine->layerCount--;
            if (engine->layerCount<1)
                engine->layerCount=1;
            else if (engine->layerCount>MAX_LAYERS)
                engine->layerCount=MAX_LAYERS;
            LOGI("Using %d layers", engine->layerCount);
        }
//        if ((keycode==AKEYCODE_DPAD_LEFT || keycode==AKEYCODE_DPAD_RIGHT) && action == AKEY_EVENT_ACTION_DOWN) {
//            if (keycode == AKEYCODE_DPAD_RIGHT)
//                engine->boxCount+=50;
//            else
//                engine->boxCount-=50;
//            if (engine->boxCount<50)
//                engine->boxCount=50;
//            else if (engine->boxCount>MAX_BOXES)
//                engine->boxCount=MAX_BOXES;
//            LOGI("Drawing %d boxes", engine->boxCount);
//            engine->rebuildCommadBuffersRequired=true;
//        }
        if ((keycode==AKEYCODE_DPAD_LEFT || keycode==AKEYCODE_DPAD_RIGHT) && action == AKEY_EVENT_ACTION_DOWN) {
            if (keycode == AKEYCODE_DPAD_RIGHT)
                engine->displayLayer++;
            else
                engine->displayLayer--;
            if (engine->displayLayer<0)
            {
                engine->displayLayer=-1;
                LOGI("Displaying all layers");
            }
            else
                LOGI("Displaying only layer %d", engine->displayLayer);
        }
        if (keycode==AKEYCODE_BACK && action == AKEY_EVENT_ACTION_UP) {
            ANativeActivity_finish(engine->app->activity);
        }
        return 1;
    }
    return 0;
}

/**
 * Process the next main command.
 */
static void engine_handle_cmd(struct android_app* app, int32_t cmd) {
    struct engine* engine = (struct engine*)app->userData;
    switch (cmd) {
        case APP_CMD_SAVE_STATE:
            // The system has asked us to save our current state.  Do so.
            engine->app->savedState = malloc(sizeof(struct saved_state));
            *((struct saved_state*)engine->app->savedState) = engine->state;
            engine->app->savedStateSize = sizeof(struct saved_state);
            break;
        case APP_CMD_INIT_WINDOW:
            // The window is being shown, get it ready.
            if (engine->app->window != NULL) {
                engine_init_display(engine);
                engine_draw_frame(engine);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // The window is being hidden or closed, clean it up.
            engine_term_display(engine);
            break;
        case APP_CMD_GAINED_FOCUS:
            LOGI("APP_CMD_GAINED_FOCUS");
            // When our app gains focus, we start monitoring the accelerometer.
//            if (engine->accelerometerSensor != NULL) {
//                ASensorEventQueue_enableSensor(engine->sensorEventQueue,
//                                               engine->accelerometerSensor);
//                // We'd like to get 60 events per second (in us).
//                ASensorEventQueue_setEventRate(engine->sensorEventQueue,
//                                               engine->accelerometerSensor,
//                                               (1000L/60)*1000);
//            }
            engine->animating = 1;
            break;
        case APP_CMD_LOST_FOCUS:
            // When our app loses focus, we stop monitoring the accelerometer.
            // This is to avoid consuming battery while not being used.
            if (engine->accelerometerSensor != NULL) {
                ASensorEventQueue_disableSensor(engine->sensorEventQueue,
                                                engine->accelerometerSensor);
            }
            // Also stop animating.
            LOGI("APP_CMD_LOST_FOCUS");
            engine->animating = 0;
            engine_draw_frame(engine);
            break;
    }
}

/**
 * This is the main entry point of a native application that is using
 * android_native_app_glue.  It runs in its own thread, with its own
 * event loop for receiving input events and doing other things.
 */
void android_main(struct android_app* state) {
    struct engine engine;

    // Make sure glue isn't stripped.
    app_dummy();

    memset(&engine, 0, sizeof(engine));
    state->userData = &engine;
    state->onAppCmd = engine_handle_cmd;
    state->onInputEvent = engine_handle_input;
    engine.app = state;
    engine.animating=1;
    engine.vulkanSetupOK=false;
    engine.frameRateClock=new btClock;
    engine.frameRateClock->reset();
    engine.simulation = new Simulation;
    engine.simulation->step();
    engine.splitscreen = true;
    engine.rebuildCommadBuffersRequired = false;
    engine.displayLayer=-1;
    engine.layerCount=4;
    engine.boxCount=100;


    // Prepare to monitor accelerometer
//    engine.sensorManager = ASensorManager_getInstance();
//    engine.accelerometerSensor = ASensorManager_getDefaultSensor(
//                                        engine.sensorManager,
//                                        ASENSOR_TYPE_ACCELEROMETER);
//    engine.sensorEventQueue = ASensorManager_createEventQueue(
//                                    engine.sensorManager,
//                                    state->looper, LOOPER_ID_USER,
//                                    NULL, NULL);

    if (state->savedState != NULL) {
        // We are starting with a previous saved state; restore from it.
        engine.state = *(struct saved_state*)state->savedState;
    }

    // loop waiting for stuff to do.

    while (1) {
        // Read all pending events.
        int ident;
        int events;
        struct android_poll_source* source;

        // If not animating, we will block forever waiting for events.
        // If animating, we loop until all events are read, then continue
        // to draw the next frame of animation.
//        LOGI("Polling. animating: %s", engine.animating ? "true" : "false");
        while ((ident=ALooper_pollAll(engine.animating ? 0 : -1, NULL, &events,
                                      (void**)&source)) >= 0) {
//            LOGI("Poll returned. animating: %s", engine.animating ? "true" : "false");

            // Process this event.
            if (source != NULL) {
                source->process(state, source);
            }

            // If a sensor has data, process it now.
//            if (ident == LOOPER_ID_USER) {
//                if (engine.accelerometerSensor != NULL) {
//                    ASensorEvent event;
//                    while (ASensorEventQueue_getEvents(engine.sensorEventQueue,
//                                                       &event, 1) > 0) {
//                        LOGI("accelerometer: x=%f y=%f z=%f",
//                             event.acceleration.x, event.acceleration.y,
//                             event.acceleration.z);
//                    }
//                }
//            }

            // Check if we are exiting.
            if (state->destroyRequested != 0) {
                engine_term_display(&engine);
                return;
            }
        }
//        LOGI("Cont");

        if (engine.animating) {
            // Done with events; draw next animation frame.
            engine.state.angle += .01f;
            if (engine.state.angle > 1) {
                engine.state.angle = 0;
            }

            // Drawing is throttled to the screen update rate, so there
            // is no need to do timing here.
//            LOGI("calling engine_draw_frame");
            engine_draw_frame(&engine);
            engine.simulation->step();
        }
    }
}
#endif
//END_INCLUDE(all)

#ifndef __ANDROID__
int main()
{
    struct engine engine;
    engine.width=800;
    engine.height=600;
//    engine.width=1920;
//    engine.height=1080;
//    engine.width=4096;
//    engine.height=2160;
    engine.animating=1;
    engine.vulkanSetupOK=false;
    engine.frameRateClock=new btClock;
    engine.frameRateClock->reset();
    engine.simulation = new Simulation;
    engine.simulation->step();
    engine.splitscreen = false;
    engine.rebuildCommadBuffersRequired = false;
    engine.displayLayer=1;
    engine.layerCount=4;
    engine.boxCount=100;

    //Setup XCB Connection:
    const xcb_setup_t *setup;
    xcb_screen_iterator_t iter;
    int scr;
    engine.xcbConnection= xcb_connect(NULL, &scr);
    if (engine.xcbConnection == NULL) {
      printf("Cannot find a compatible Vulkan ICD.\n");
      return -1;
    }

    setup = xcb_get_setup(engine.xcbConnection);
    iter = xcb_setup_roots_iterator(setup);
    while (scr-- > 0)
      xcb_screen_next(&iter);
    xcb_screen_t *screen = iter.data;

    //Create an xcb window:
    uint32_t value_mask, value_list[32];

    engine.window = xcb_generate_id(engine.xcbConnection);

    value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    value_list[0] = screen->black_pixel;
    value_list[1] = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS;

    xcb_create_window(engine.xcbConnection, XCB_COPY_FROM_PARENT, engine.window,
                      screen->root, 0, 0, engine.width, engine.height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      value_mask, value_list);

    //We want to know when the user presses the close button:
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(engine.xcbConnection, 1, 12, "WM_PROTOCOLS");
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(engine.xcbConnection, cookie, 0);

    xcb_intern_atom_cookie_t cookie2 = xcb_intern_atom(engine.xcbConnection, 0, 16, "WM_DELETE_WINDOW");
    xcb_intern_atom_reply_t* delete_window_reply = xcb_intern_atom_reply(engine.xcbConnection, cookie2, 0);

    xcb_change_property(engine.xcbConnection, XCB_PROP_MODE_REPLACE, engine.window, (*reply).atom, 4, 32, 1, &(*delete_window_reply).atom);
    char const* windowTitle="Vulkan depth peel demo";
    xcb_change_property(engine.xcbConnection, XCB_PROP_MODE_REPLACE, engine.window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(windowTitle), windowTitle);

    //This demo does not include the code necessary to handle resizing the framebuffers
    //so lets ask the window manager to disable window resizing.
    xcb_size_hints_t hints;
    hints.flags=0;
    xcb_icccm_size_hints_set_min_size(&hints, engine.width, engine.height);
    xcb_icccm_size_hints_set_max_size(&hints, engine.width, engine.height);
    xcb_icccm_set_wm_size_hints(engine.xcbConnection, engine.window, XCB_ATOM_WM_NORMAL_HINTS, &hints);

    xcb_map_window(engine.xcbConnection, engine.window);
    xcb_flush(engine.xcbConnection);

      //Wait until the window has been exposed:
    xcb_generic_event_t *e;
    while ((e = xcb_wait_for_event(engine.xcbConnection))) {
      if ((e->response_type & ~0x80) == XCB_EXPOSE)
        break;
    }

    engine_init_display(&engine);
    bool done=false;
//    for (int i=0; i<4; i++) {
    while (!(done==1)) {
        while (1==1) {
            e = xcb_poll_for_event(engine.xcbConnection);
            if (!e) break;
            printf("Event: ");
            switch(e->response_type & ~0x80)
            {
            case XCB_EXPOSE:
                break;
            case XCB_EVENT_MASK_BUTTON_PRESS:
                done=1;
                break;
            case XCB_KEY_PRESS:
            {
                xcb_keycode_t key = ((xcb_key_press_event_t*)e)->detail;
//                LOGI("Key pressed %d", key);
                if (key == 9)
                    done=1;
                if (key == 25 || key == 39)
                {
                    if (key == 25)
                        engine.displayLayer++;
                    else
                        engine.displayLayer--;
                    if (engine.displayLayer<0)
                    {
                        engine.displayLayer=-1;
                        LOGI("Displaying all layers");
                    }
                    else
                        LOGI("Displaying only layer %d", engine.displayLayer);
                }
                else if (key == 111 || key == 116)
                {
                    if (key == 111)
                        engine.layerCount++;
                    else
                        engine.layerCount--;
                    if (engine.layerCount<1)
                        engine.layerCount=1;
                    else if (engine.layerCount>MAX_LAYERS)
                        engine.layerCount=MAX_LAYERS;
                    LOGI("Using %d layers", engine.layerCount);
                }
                else if (key == 113 || key == 114)
                {
                    if (key == 114)
                        engine.boxCount+=50;
                    else
                        engine.boxCount-=50;
                    if (engine.boxCount<50)
                        engine.boxCount=50;
                    else if (engine.boxCount>MAX_BOXES)
                        engine.boxCount=MAX_BOXES;
                    LOGI("Drawing %d boxes", engine.boxCount);
                    engine.rebuildCommadBuffersRequired=true;
                }
                else if (key == 33)
                    engine.simulation->paused= !engine.simulation->paused;
                else if(key == 65)
                {
                    engine.splitscreen = !engine.splitscreen;
                    engine.rebuildCommadBuffersRequired=true;
                }
            }
                break;
            default:
                printf ("Unknown XCB Event %d\n", (e->response_type & ~0x80));
            }
            if ((e->response_type & ~0x80)==XCB_CLIENT_MESSAGE)
            {
                printf("XCB_CLIENT_MESSAGE");
                if(((xcb_client_message_event_t*)e)->data.data32[0] == delete_window_reply->atom)
                    done=1;
            }
            free(e);
        }
        if (done)
            printf("done\n");
        engine_draw_frame(&engine);
        engine.simulation->step();
    }
    return 0;
}

#endif
