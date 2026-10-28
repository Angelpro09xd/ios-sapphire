/*
 * libcc_kext.a for linking xnu with LLVM clang on Linux. Apple's clang ships libclang_rt.cc_kext.a, whose
 * profile runtime OSKext.cpp calls to hand a kext's PGO counters to userspace (OSKextGrabPgoData); a kernel
 * built without -fprofile-instr-generate has none, so these report an empty profile.
 */
unsigned long long
__llvm_profile_get_size_for_buffer_internal(const char *DataBegin, const char *DataEnd,
    const char *CountersBegin, const char *CountersEnd, const char *NamesBegin, const char *NamesEnd)
{
	return 0;
}

int
__llvm_profile_write_buffer_internal(char *Buffer, const char *DataBegin, const char *DataEnd,
    const char *CountersBegin, const char *CountersEnd, const char *NamesBegin, const char *NamesEnd)
{
	return -1;
}
