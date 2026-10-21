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
static int sapphire_logged;

#define SLOG(...) do { fprintf(stderr, "SAPPHIRE MTL[%d]: ", getpid()); fprintf(stderr, __VA_ARGS__); \
	fputc('\n', stderr); } while (0)

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

@implementation SapphireMTLDevice

+ (void)load
{
	void *spi = objc_getProtocol("MTLDeviceSPI"), *pub = objc_getProtocol("MTLDevice");
	Class cls = objc_getClass("SapphireMTLDevice");
	if (spi != NULL) {
		route_protocol(cls, spi, 0);
	}
	if (pub != NULL) {
		route_protocol(cls, pub, 0);
	}
	SLOG("loaded, MTLDeviceSPI %s, MTLDevice %s", spi ? "found" : "MISSING", pub ? "found" : "MISSING");
}

- (id)initWithAcceleratorPort:(unsigned int)port
{
	self = [super init];
	sapphire_port = port;
	SLOG("initWithAcceleratorPort: %u -> %p", port, (void *)self);
	return self;
}

/* Everything routed to _objc_msgForward ends up here, as an NSInvocation. */
- (void)forwardInvocation:(id)inv
{
	SEL sel = ((SEL (*)(id, SEL))objc_msgSend)(inv, sel_registerName("selector"));
	id sig = msg0(inv, "methodSignature");
	unsigned long rlen = ((unsigned long (*)(id, SEL))objc_msgSend)(sig, sel_registerName("methodReturnLength"));
	Class super_cls = class_getSuperclass(objc_getClass("SapphireMTLDevice"));
	Method m = class_getInstanceMethod(super_cls, sel);

	if (sapphire_logged++ < 4000) {
		SLOG("%s%s", sel_getName(sel), m != NULL ? "  (to _MTLDevice)" : "");
	}
	if (rlen > 0) {
		void *zero = calloc(1, rlen < 64 ? 64 : rlen);
		((void (*)(id, SEL, void *))objc_msgSend)(inv, sel_registerName("setReturnValue:"), zero);
		free(zero);
	}
	if (m != NULL) {
		((void (*)(id, SEL, IMP_t))objc_msgSend)(inv, sel_registerName("invokeUsingIMP:"), method_getImplementation(m));
	}
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
