/*
 * SapphireNoCamera: the call screen asks for cameras that do not answer.
 *
 * When a call ends, InCallService destroys its call scene and -[TUVideoDeviceController stopPreview]
 * asks TelephonyUtilities for the video input devices (-[TUVideoDeviceControllerProvider inputDevices]
 * -> AVCaptureDeviceDiscoverySession -> FigCaptureSourceRemoteCopyCaptureSources), which waits on
 * mediaserverd's capture server; there is no iOS camera on sapphire and that wait never ends. Its main
 * thread stuck, InCallService took no more tel: URLs: the first call worked, the dial button then did
 * nothing. Here the provider has no video input devices, at once.
 */
#include <objc/runtime.h>
#include <objc/message.h>

static id
no_input_devices(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSArray"), sel_registerName("array"));
}

__attribute__((constructor)) static void
install(void)
{
	Class provider = objc_getClass("TUVideoDeviceControllerProvider");
	Method method = provider != Nil ? class_getInstanceMethod(provider, sel_registerName("inputDevices")) : NULL;

	if (method != NULL) {
		method_setImplementation(method, (IMP)no_input_devices);
	}
}
