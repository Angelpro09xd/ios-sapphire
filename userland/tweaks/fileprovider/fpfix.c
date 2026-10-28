/*
 * libsapphire-fpfix.dylib: "On My iPhone" in the Files app (com.apple.DocumentsApp).
 *
 * Files showed nothing: "Wanted to use com.apple.FileProvider.LocalStorage, but it isn't available",
 * "Could not set domain cache ... because storageURLs is empty". Four things stood in the way, each
 * fixed here by a hook that acts only in a process that has the class it needs (FileProvider and
 * PlugInKit, 20H392) and lets Apple's code run whenever it would have succeeded on its own:
 *
 * 1. fileproviderd had no domains. It keeps every provider's domains in an FP library on a volume that
 *    -[FPDVolume isEligibleForFPFS:] accepts: only a local, writable volume whose statfs f_fstypename is
 *    "apfs". Ours is HFS+ ("Non-eligible device /dev/ufsroot0: non-apfs"), so no library, no
 *    -[FPDProvider importDomainsFromLibrary:updatePlist:], no default domain for any provider, and
 *    -[FPDProvider providedItemsURLs] (the union of its domains' rootURLs), which Files gets as the
 *    domain's storageURLs, was empty. Here a local, writable "hfs" volume is accepted too (the method
 *    is called only from -[FPDVolume initWithVolumeInfo:knownRole:volumeManager:]).
 *
 * 2. The LocalStorage extension died at start. Every PlugInKit extension checks
 *    (-[PKService _prepareToRun] -> -[PKService checkEnvironment:]) that sandbox_container_path_for_pid()
 *    gives it its container and throws ("Bootstrapping; failed to get my own sandbox container")
 *    otherwise; with no Sandbox.kext the kernel's stand-in policy (xnu 0050) answers op 4 with ENOENT.
 *    Here the check is skipped when there is no container to give (it only verifies; nothing uses it).
 *
 * 3. File URLs could not be handed over. FPSandboxingURLWrapper wraps every URL a provider or the daemon
 *    gives out with a sandbox extension from -[NSURL fp_issueSandboxExtensionOfClass:report:error:];
 *    issuing one fails here (op 5, ENOENT), the wrapper is nil and URLForItemID fails ("Could not issue
 *    com.apple.app-sandbox.read-write sandbox extension", Cocoa 4101). Nothing is sandboxed on this port,
 *    so no token is needed to reach the file: for a path that exists a placeholder token in the usual
 *    layout is given instead (consuming it just fails in startAccessingSecurityScopedResource).
 *
 * Loaded into fileproviderd by fpstart and into LocalStorageFileProvider by lswrap (launchd drops DYLD_*
 * for daemons and services, see fpstart.c / lswrap.c). Log: /var/tmp/fpfix.log.
 */
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

extern int sandbox_container_path_for_pid(pid_t pid, char *buffer, size_t bufsize);

static void
say(const char *what, const char *detail)
{
	int fd = open("/var/tmp/fpfix.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		char line[512];
		int n = snprintf(line, sizeof(line), "[%d %s] %s%s%s\n", getpid(), getprogname(), what,
		    detail != NULL ? ": " : "", detail != NULL ? detail : "");
		if (n > 0) {
			write(fd, line, strlen(line));
		}
		close(fd);
	}
}

/* 1. fileproviderd: an HFS+ volume may hold the FP library */
typedef BOOL (*eligible_fn)(id, SEL, struct statfs *);
static eligible_fn eligible_original;

static BOOL
eligible(id self, SEL selector, struct statfs *sfs)
{
	if (eligible_original(self, selector, sfs)) {
		return YES;
	}
	if (sfs != NULL && strcmp(sfs->f_fstypename, "hfs") == 0 && (sfs->f_flags & MNT_LOCAL) != 0 &&
	    (sfs->f_flags & MNT_RDONLY) == 0) {
		say("FP library allowed on hfs volume", sfs->f_mntonname);
		return YES;
	}
	return NO;
}

/* 2. PlugInKit extensions: no sandbox container to check */
typedef void (*check_fn)(id, SEL, id);
static check_fn check_original;

static void
check_environment(id self, SEL selector, id arg)
{
	char path[1024] = "";

	if (sandbox_container_path_for_pid(getpid(), path, sizeof(path)) == 0 && strlen(path) > 1) {
		check_original(self, selector, arg);
		return;
	}
	say("no sandbox container, -[PKService checkEnvironment:] skipped", NULL);
}

/* 3. a placeholder sandbox extension token for a file that exists */
typedef id (*issue_fn)(id, SEL, const char *, BOOL, id *);   /* class: a C string (sandbox_extension_issue_file) */
static issue_fn issue_original;
static int issue_logged;

static id
issue_extension(id self, SEL selector, const char *extension_class, BOOL report, id *error)
{
	id token = issue_original(self, selector, extension_class, report, error);
	if (token != nil) {
		return token;
	}
	id path = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("path"));
	const char *cpath = path != nil ?
	    ((const char *(*)(id, SEL))objc_msgSend)(path, sel_registerName("fileSystemRepresentation")) : NULL;
	const char *cclass = extension_class;
	if (cpath == NULL || access(cpath, F_OK) != 0) {
		return nil;
	}
	char buf[1400];
	int n = snprintf(buf, sizeof(buf), "%064d;00;00000000;00000000;00000000;0000000000000000;%s;01;00000000;0000000000000000;%s",
	    0, cclass != NULL ? cclass : "com.apple.app-sandbox.read", cpath);
	if (n <= 0 || (size_t)n >= sizeof(buf)) {
		return nil;
	}
	if (!issue_logged) {
		issue_logged = 1;
		say("sandbox extension not issued, placeholder token given (first one)", cpath);
	}
	if (error != NULL) {
		*error = nil;
	}
	return ((id (*)(id, SEL, const void *, unsigned long))objc_msgSend)((id)objc_getClass("NSData"),
	    sel_registerName("dataWithBytes:length:"), buf, (unsigned long)n + 1);
}

/*
 * 4. APFS-only fsctls. FileProvider's fpfs_* helpers ask the volume through APFS fsctls ('J' group) about
 *    sync roots, eviction and directory stats; HFS+ answers ENOTTY. fpfs_fgetfileattrs (every item
 *    lookup of LocalStorage) starts with fpfs_fget_parent_syncroot and gives up on any error, so the
 *    root itself was "no such item" ("[25: Inappropriate ioctl for device]", FP -1005) and the domain
 *    listed nothing. The helpers call each other directly inside FileProvider (no stub dyld could
 *    interpose), so each is redirected at its entry (16-byte trampoline, the first four instructions
 *    checked first, as bdsandbox does) to a copy that asks the same fsctl and, on ENOTTY only, answers
 *    what a volume without these features means: no sync root, not evictable, dirstat unsupported
 *    (enabling: EINVAL, which fpfs_track_document accepts; reading: ENOTSUP, which fpfs_fgethandle
 *    accepts for directories), setting a sync root a no-op; errno is left as it was.
 */
#include <errno.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/xattr.h>

extern int ffsctl(int fd, unsigned long cmd, void *data, unsigned int options);

#define APFSIOC_GET_SYNC_ROOT           0x40044a48UL
#define APFSIOC_SET_SYNC_ROOT           0x80044a48UL
#define APFSIOC_GET_PARENT_SYNC_ROOT    0x40084a4aUL
#define APFSIOC_IS_EVICTABLE            0x40084a47UL
#define APFSIOC_DIR_STATS_ENABLE        0x80084a02UL
#define APFSIOC_DIR_STATS_OP            0xc0284a20UL

static int
hfs_fget_parent_syncroot(int fd, uint64_t *root)
{
	uint64_t v = 0;
	int saved = errno;
	if (ffsctl(fd, APFSIOC_GET_PARENT_SYNC_ROOT, &v, 0) < 0) {
		if (errno != ENOTTY) {
			return -1;
		}
		errno = saved;
		v = 0;
	}
	if (root != NULL) {
		*root = v;
	}
	return 0;
}

static int
hfs_fget_syncroot(int fd, bool *is_root)
{
	uint32_t v = 0;
	int saved = errno;
	if (ffsctl(fd, APFSIOC_GET_SYNC_ROOT, &v, 0) < 0) {
		if (errno != ENOTTY) {
			return -1;
		}
		errno = saved;
		v = 0;
	}
	if (is_root != NULL) {
		*is_root = v != 0;
	}
	return 0;
}

static int
hfs_fset_syncroot(int fd, int set)
{
	uint32_t v = set != 0;
	int saved = errno;
	if (ffsctl(fd, APFSIOC_SET_SYNC_ROOT, &v, 0) < 0) {
		if (errno != ENOTTY) {
			return -1;
		}
		errno = saved;
	}
	return 0;
}

static int
hfs_is_evictable(int fd, bool *evictable)
{
	uint64_t v = 0;
	bool e = false;
	int saved = errno;
	if (ffsctl(fd, APFSIOC_IS_EVICTABLE, &v, 0) < 0) {
		if (errno != EPERM && errno != ENOTSUP && errno != ENOTTY) {
			return -1;
		}
		errno = saved;
	} else {
		e = ((v >> 8) & 1) != 0;
	}
	if (evictable != NULL) {
		*evictable = e;
	}
	return 0;
}

static int
hfs_pkg_enable_dirstat(int fd)
{
	uint64_t v = 0;
	int r = ffsctl(fd, APFSIOC_DIR_STATS_ENABLE, &v, 0);
	if (r < 0 && errno == ENOTTY) {
		errno = EINVAL;
	}
	return r;
}

/* the APFS directory statistics a caller asks for; on ENOTTY answer ENOTSUP, as for a demoted package,
 * which fpfs_fgethandle accepts (without this every directory failed: "untitled folder" in Files) */
static int
hfs_pkg_dirstat(int fd, uint64_t *size, uint32_t *gencount)
{
	struct {
		uint64_t flags, reserved[2], total_size;
		uint32_t gencount, pad[3];
	} stats = { .flags = 1 };

	if (fgetxattr(fd, "com.apple.fileprovider.dir#N", NULL, 0, 0, 0) >= 0) {
		errno = ENOTSUP;        /* demoted package */
		return -1;
	}
	if (errno != ENOATTR) {
		return -1;
	}
	if (ffsctl(fd, APFSIOC_DIR_STATS_OP, &stats, 0) < 0) {
		if (errno == ENOTTY) {
			errno = ENOTSUP;
		}
		return -1;
	}
	if (size != NULL) {
		*size = stats.total_size;
	}
	if (gencount != NULL) {
		*gencount = stats.gencount;
	}
	return 0;
}

static const struct {
	const char *name;
	uint32_t head[4];
	void *replacement;
} redirects[] = {
	{ "fpfs_fget_parent_syncroot", { 0xd100c3ff, 0xa9014ff4, 0xa9027bfd, 0x910083fd }, (void *)hfs_fget_parent_syncroot },
	{ "fpfs_fget_syncroot",        { 0xd100c3ff, 0xa9014ff4, 0xa9027bfd, 0x910083fd }, (void *)hfs_fget_syncroot },
	{ "fpfs_fset_syncroot",        { 0xd100c3ff, 0xa9014ff4, 0xa9027bfd, 0x910083fd }, (void *)hfs_fset_syncroot },
	{ "fpfs_is_evictable",         { 0xd100c3ff, 0xa9014ff4, 0xa9027bfd, 0x910083fd }, (void *)hfs_is_evictable },
	{ "fpfs_pkg_enable_dirstat",   { 0xd10083ff, 0xa9017bfd, 0x910043fd, 0xf90007ff }, (void *)hfs_pkg_enable_dirstat },
	{ "fpfs_pkg_dirstat",          { 0xd10183ff, 0xa90357f6, 0xa9044ff4, 0xa9057bfd }, (void *)hfs_pkg_dirstat },
};

static int
redirect(uint32_t *p, void *target)
{
	vm_address_t start = (vm_address_t)p & ~(vm_address_t)(vm_page_size - 1);
	vm_size_t size = ((vm_address_t)p + 16 + vm_page_size - 1 - start) & ~(vm_size_t)(vm_page_size - 1);

	if (vm_protect(mach_task_self(), start, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		return -1;
	}
	p[0] = 0x58000050;      /* ldr x16, #8 */
	p[1] = 0xd61f0200;      /* br x16 */
	memcpy(&p[2], &target, sizeof(target));
	vm_protect(mach_task_self(), start, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(p, 16);
	return 0;
}

static void
redirect_apfs_helpers(void)
{
	for (size_t i = 0; i < sizeof(redirects) / sizeof(redirects[0]); i++) {
		uint32_t *p = dlsym(RTLD_DEFAULT, redirects[i].name);
		if (p == NULL) {
			continue;
		}
		if (memcmp(p, redirects[i].head, sizeof(redirects[i].head)) != 0) {
			say("unexpected code, not redirected", redirects[i].name);
			continue;
		}
		say(redirect(p, redirects[i].replacement) == 0 ? "redirected" : "vm_protect failed", redirects[i].name);
	}
}

static IMP
replace(const char *class_name, const char *selector, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls != Nil ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (method == NULL) {
		return NULL;
	}
	say("hooked", selector);
	return method_setImplementation(method, replacement);
}

__attribute__((constructor)) static void
install(void)
{
	unsetenv("DYLD_INSERT_LIBRARIES");      /* this process only, not what it spawns */
	eligible_original = (eligible_fn)replace("FPDVolume", "isEligibleForFPFS:", (IMP)eligible);
	check_original = (check_fn)replace("PKService", "checkEnvironment:", (IMP)check_environment);
	redirect_apfs_helpers();
	issue_original = (issue_fn)replace("NSURL", "fp_issueSandboxExtensionOfClass:report:error:", (IMP)issue_extension);
}
