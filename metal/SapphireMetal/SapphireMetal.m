/*
 * SapphireMetal: a Metal driver for the Adreno 610, step 0 — a device that tells us what iOS asks
 * of one.
 *
 * Metal finds its driver through the accelerator service in the IORegistry
 * (getMetalPluginClassForService): MetalPluginName names a bundle in /System/Library/Extensions,
 * MetalPluginClassName a class in it, which must be a subclass of _MTLDevice, and the device is
 * [[class alloc] initWithAcceleratorPort:port]. _MTLDevice's own initWithAcceleratorPort: is
 * abstract; Apple's IOGPUMetalDevice does [super init] and then opens its kernel interface. This
 * one does [super init] only.
 *
 * Every instance method of the MTLDeviceSPI protocol (which takes in MTLDevice) is routed through
 * forwardInvocation:, which writes the selector to stderr (the kernel console shows it, prefixed
 * with the process name), then hands the call to _MTLDevice when it has an implementation, and
 * otherwise answers zero. Abstract methods end in doesNotRecognizeSelector:, which logs and
 * answers zero instead of raising. The result is the list, in order, of what CoreAnimation and
 * the rest of iOS want from a Metal device, which is what the real driver has to implement.
 *
 * Built on Linux: clang -target arm64-apple-ios16.0, ld64.lld against the stubs in ../tbd, ad-hoc
 * signed (see build.sh). No Apple headers: what is needed is declared here.
 */
/* Darwin's libc, declared here (Linux's headers do not describe it) */
typedef unsigned long size_t;
typedef struct __sFILE FILE;
extern FILE *__stderrp;
#define stderr __stderrp
#define NULL ((void *)0)
int fprintf(FILE *, const char *, ...) __attribute__((format(printf, 2, 3)));
int fputc(int, FILE *);
int getpid(void);
void *calloc(size_t, size_t);
void free(void *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strstr(const char *, const char *);

typedef struct objc_method *Method;
typedef struct objc_protocol_def Protocol_t;
struct objc_method_description {
	SEL name;
	const char *types;
};
typedef void (*IMP_t)(void);

extern id objc_msgSend(id, SEL, ...);
struct objc_super {
	id receiver;
	Class super_class;
};
extern id objc_msgSendSuper(struct objc_super *, SEL, ...);
extern Class objc_getClass(const char *);
extern void *objc_getProtocol(const char *);
extern struct objc_method_description *protocol_copyMethodDescriptionList(void *p, int required, int instance,
    unsigned int *count);
extern void **protocol_copyProtocolList(void *p, unsigned int *count);
extern const char *protocol_getName(void *p);
extern int class_addMethod(Class cls, SEL name, IMP_t imp, const char *types);
extern int class_addProtocol(Class cls, void *protocol);
extern Class objc_allocateClassPair(Class superclass, const char *name, size_t extra);
extern void objc_registerClassPair(Class cls);
extern const char *class_getName(Class cls);
extern Class object_getClass(id obj);
int snprintf(char *, size_t, const char *, ...);
extern Method class_getInstanceMethod(Class cls, SEL name);
extern IMP_t method_getImplementation(Method m);
extern Class class_getSuperclass(Class cls);
extern const char *sel_getName(SEL sel);
extern SEL sel_registerName(const char *str);
extern void _objc_msgForward(void);

__attribute__((objc_root_class))
@interface NSObject
@end

@interface _MTLDevice : NSObject
- (id)init;
@end

@interface SapphireMTLDevice : _MTLDevice
@end

static unsigned int sapphire_port;
static id sapphire_device;
static int sapphire_logged;

/* one fprintf per line: the console splits a line written in several pieces */
#define SLOG(fmt, ...) fprintf(stderr, "SAPPHIRE MTL[%d]: " fmt "\n", getpid(), ##__VA_ARGS__)

static id
msg0(id obj, const char *sel)
{
	return ((id (*)(id, SEL))objc_msgSend)(obj, sel_registerName(sel));
}

static void
route_protocol(Class cls, void *proto, int depth)
{
	unsigned int n = 0;

	for (int required = 1; required >= 0; required--) {
		struct objc_method_description *m = protocol_copyMethodDescriptionList(proto, required, 1, &n);
		for (unsigned int i = 0; i < n; i++) {
			const char *name = sel_getName(m[i].name);
			if (strcmp(name, "initWithAcceleratorPort:") == 0 || strncmp(name, "init", 4) == 0 ||
			    strcmp(name, "dealloc") == 0 || strcmp(name, "retain") == 0 || strcmp(name, "release") == 0 ||
			    strcmp(name, "autorelease") == 0 || strcmp(name, "retainCount") == 0 ||
			    strcmp(name, "respondsToSelector:") == 0 || strcmp(name, "conformsToProtocol:") == 0 ||
			    strcmp(name, "isKindOfClass:") == 0 || strcmp(name, "class") == 0 || strcmp(name, "hash") == 0 ||
			    strcmp(name, "isEqual:") == 0 || strcmp(name, "description") == 0 ||
			    strcmp(name, "debugDescription") == 0 || strcmp(name, "zone") == 0) {
				continue;
			}
			(void)class_addMethod(cls, m[i].name, (IMP_t)_objc_msgForward, m[i].types);
		}
		free(m);
	}
	if (depth < 4) {
		unsigned int np = 0;
		void **sub = protocol_copyProtocolList(proto, &np);
		for (unsigned int i = 0; i < np; i++) {
			if (strcmp(protocol_getName(sub[i]), "NSObject") != 0) {
				route_protocol(cls, sub[i], depth + 1);
			}
		}
		free(sub);
	}
}

/*
 * Step 0b: objects of the right kind. When a routed method returns an object, the selector tells
 * what CoreAnimation expects back (newCommandQueue -> an MTLCommandQueue, commandBuffer -> an
 * MTLCommandBuffer, newTexture... -> an MTLTexture, ...), and it gets a stub of that kind: a
 * class made at run time per protocol, declaring the protocol (and its SPI twin) and routing all
 * of its methods through the same logging forwardInvocation. Nothing is drawn; the point is to
 * see the whole of what is asked, not only up to the first nil.
 */
static const struct {
	const char *needle, *proto;
} stub_kinds[] = {
	{ "parallelRenderCommandEncoder", "MTLParallelRenderCommandEncoder" },
	{ "renderCommandEncoder", "MTLRenderCommandEncoder" },
	{ "blitCommandEncoder", "MTLBlitCommandEncoder" },
	{ "computeCommandEncoder", "MTLComputeCommandEncoder" },
	{ "newCommandQueue", "MTLCommandQueue" },
	{ "commandBuffer", "MTLCommandBuffer" },
	{ "newRenderPipelineState", "MTLRenderPipelineState" },
	{ "newComputePipelineState", "MTLComputePipelineState" },
	{ "newDepthStencilState", "MTLDepthStencilState" },
	{ "newSamplerState", "MTLSamplerState" },
	{ "newTexture", "MTLTexture" },
	{ "newBuffer", "MTLBuffer" },
	{ "newDefaultLibrary", "MTLLibrary" },
	{ "newLibrary", "MTLLibrary" },
	{ "newFunction", "MTLFunction" },
	{ "newSharedEvent", "MTLSharedEvent" },
	{ "newEvent", "MTLEvent" },
	{ "newFence", "MTLFence" },
	{ "newHeap", "MTLHeap" },
	{ "newIndirectCommandBuffer", "MTLIndirectCommandBuffer" },
	{ "newArgumentEncoder", "MTLArgumentEncoder" },
	{ "newPipelineLibrary", "MTLPipelineLibrary" },
	{ "newBinaryArchive", "MTLBinaryArchive" },
	{ "newDynamicLibrary", "MTLDynamicLibrary" },
	{ "newRasterizationRateMap", "MTLRasterizationRateMap" },
	{ "newCounterSampleBuffer", "MTLCounterSampleBuffer" },
	{ "newAccelerationStructure", "MTLAccelerationStructure" },
	{ "newResidencySet", "MTLResidencySet" },
	{ NULL, NULL }
};

static void stub_forward(id self, SEL _cmd, id inv);
static id stub_signature(id self, SEL _cmd, SEL sel);
static id stub_unrecognized(id self, SEL _cmd, SEL sel);

static int
contains(const char *hay, const char *needle)
{
	return strstr(hay, needle) != NULL;
}

static Class
stub_class(const char *proto_name)
{
	char name[160], spi_name[160];
	snprintf(name, sizeof(name), "SapphireStub_%s", proto_name);
	Class cls = objc_getClass(name);
	if (cls != (Class)0) {
		return cls;
	}
	cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);
	if (cls == (Class)0) {
		return objc_getClass(name);
	}
	snprintf(spi_name, sizeof(spi_name), "%sSPI", proto_name);
	void *spi = objc_getProtocol(spi_name), *pub = objc_getProtocol(proto_name);
	if (spi != NULL) {
		(void)class_addProtocol(cls, spi);
		route_protocol(cls, spi, 0);
	}
	if (pub != NULL) {
		(void)class_addProtocol(cls, pub);
		route_protocol(cls, pub, 0);
	}
	(void)class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP_t)stub_forward, "v@:@");
	(void)class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP_t)stub_signature, "@@::");
	(void)class_addMethod(cls, sel_registerName("doesNotRecognizeSelector:"), (IMP_t)stub_unrecognized, "@@::");
	objc_registerClassPair(cls);
	SLOG("made %s (%s%s)", name, pub ? proto_name : "no protocol", spi ? " + SPI" : "");
	return cls;
}

static id
stub_for(SEL sel)
{
	const char *n = sel_getName(sel);
	if (strcmp(n, "device") == 0) {
		return sapphire_device;
	}
	for (int i = 0; stub_kinds[i].needle != NULL; i++) {
		if (contains(n, stub_kinds[i].needle)) {
			id obj = msg0((id)stub_class(stub_kinds[i].proto), "alloc");
			return msg0(obj, "init");
		}
	}
	return (id)0;
}

/* The routed call: log, then answer with a stub, _MTLDevice's implementation, or zero. */
static void
forward_common(id self, id inv, Class super_cls)
{
	SEL sel = ((SEL (*)(id, SEL))objc_msgSend)(inv, sel_registerName("selector"));
	id sig = msg0(inv, "methodSignature");
	unsigned long rlen = ((unsigned long (*)(id, SEL))objc_msgSend)(sig, sel_registerName("methodReturnLength"));
	const char *rtype = ((const char *(*)(id, SEL))objc_msgSend)(sig, sel_registerName("methodReturnType"));
	Method m = super_cls != (Class)0 ? class_getInstanceMethod(super_cls, sel) : (Method)0;
	id stub = (rtype != NULL && rtype[0] == '@') ? stub_for(sel) : (id)0;
	/*
	 * A raw pointer (targetDeviceInfo, ...) gets zeroed memory rather than NULL, so a caller
	 * that reads a field of it carries on and the trace goes further.
	 */
	static char zeros[65536];
	int ptr = rtype != NULL && (rtype[0] == '^' || rtype[0] == '*') && rlen == sizeof(void *);
	const char *who = class_getName(object_getClass(self));

	if (sapphire_logged++ < 6000) {
		SLOG("%s %s%s", strncmp(who, "SapphireStub_", 13) == 0 ? who + 13 : "device", sel_getName(sel),
		    stub != (id)0 ? "  -> stub" : (ptr ? "  -> zeroed memory" : (m != NULL ? "  (to _MTLDevice)" : "")));
	}
	if (rlen > 0) {
		void *zero = calloc(1, rlen < 64 ? 64 : rlen);
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), zero);
		free(zero);
	}
	if (stub != (id)0) {
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), &stub);
	} else if (ptr) {
		void *z = zeros;
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), &z);
	} else if (m != NULL) {
		((void (*)(id, SEL, IMP_t))objc_msgSend)(inv, sel_registerName("invokeUsingIMP:"), method_getImplementation(m));
	}
}

static void
stub_forward(id self, SEL _cmd, id inv)
{
	forward_common(self, inv, (Class)0);
}

static id
stub_signature(id self, SEL _cmd, SEL sel)
{
	struct objc_super sup = { self, objc_getClass("NSObject") };
	id sig = ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup,
	    sel_registerName("methodSignatureForSelector:"), sel);
	if (sig == (id)0) {
		sig = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSMethodSignature"),
		    sel_registerName("signatureWithObjCTypes:"), "@@:");
	}
	return sig;
}

static id
stub_unrecognized(id self, SEL _cmd, SEL sel)
{
	if (sapphire_logged++ < 6000) {
		SLOG("%s %s: not implemented, answering 0", class_getName(object_getClass(self)), sel_getName(sel));
	}
	return (id)0;
}

@implementation SapphireMTLDevice

+ (void)load
{
	void *spi = objc_getProtocol("MTLDeviceSPI"), *pub = objc_getProtocol("MTLDevice");
	Class cls = objc_getClass("SapphireMTLDevice");
	/*
	 * MTLAddDevice asserts [newDevice conformsToProtocol:@protocol(MTLDevice)], and Apple's
	 * device classes declare <MTLDeviceSPI> (which takes in MTLDevice): declare both.
	 */
	if (spi != NULL) {
		(void)class_addProtocol(cls, spi);
		route_protocol(cls, spi, 0);
	}
	if (pub != NULL) {
		(void)class_addProtocol(cls, pub);
		route_protocol(cls, pub, 0);
	}
	SLOG("loaded, MTLDeviceSPI %s, MTLDevice %s", spi ? "found" : "MISSING", pub ? "found" : "MISSING");
}

- (id)initWithAcceleratorPort:(unsigned int)port
{
	self = [super init];
	sapphire_port = port;
	sapphire_device = self;
	SLOG("initWithAcceleratorPort: %u -> %p", port, (void *)self);
	return self;
}

/* Everything routed to _objc_msgForward ends up here, as an NSInvocation. */
- (void)forwardInvocation:(id)inv
{
	forward_common(self, inv, class_getSuperclass(objc_getClass("SapphireMTLDevice")));
}

/*
 * A selector that is in no protocol gets a generic signature (object return, no arguments), so
 * the runtime forwards it here instead of terminating the process; it is logged and answered
 * zero without looking at its arguments.
 */
- (id)methodSignatureForSelector:(SEL)sel
{
	struct objc_super sup = { self, class_getSuperclass(objc_getClass("SapphireMTLDevice")) };
	id sig = ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup,
	    sel_registerName("methodSignatureForSelector:"), sel);
	if (sig == (id)0) {
		if (sapphire_logged++ < 4000) {
			SLOG("%s: in no protocol, answering 0", sel_getName(sel));
		}
		sig = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSMethodSignature"),
		    sel_registerName("signatureWithObjCTypes:"), "@@:");
	}
	return sig;
}

/* _MTLDevice's abstract methods call this; answer zero instead of raising. */
- (id)doesNotRecognizeSelector:(SEL)sel
{
	if (sapphire_logged++ < 4000) {
		SLOG("%s: not implemented (abstract or unknown), answering 0", sel_getName(sel));
	}
	return (id)0;
}

@end
