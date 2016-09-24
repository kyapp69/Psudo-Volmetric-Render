# Vulkan depth peeled Psudo-volumetric render demo
Demonstrates a psudo-volumetric rendering technique that builds upon the depth peeling order independent transparency method.

This demo will run on Linux (XCB). It makes use of subpasses, input attachments and reusable command buffers.

Keys (on Linux):
- Up and down to change number of layers used.
- W and S to display only one of the peeled layers and to select the currently displayed layer.

![Screenshot](https://github.com/openforeveryone/VulkanDepthPeel/blob/master/ScreenShot.png "Screenshot")
