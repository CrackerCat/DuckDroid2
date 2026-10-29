// Verifies the hand-written slot indices in jni_layout.hpp against the ROM's
// real jni.h. Any drift (a new SDK adding an entry, a manual edit) fails here
// before it becomes a silent mis-dispatch in the guest.

#include <cstddef>
#include <cstdio>
#include <string_view>

#include <jni.h>

#include "runtime/jni/jni_layout.hpp"

using namespace duckemu::runtime::jni;

namespace
{

int failures = 0;

void checkOffset(const char *field, size_t actual, uint32_t slot, const char *expected_name, const char *name)
{
	if (actual != static_cast<size_t>(slot) * sizeof(void *) || expected_name != std::string_view(name))
	{
		std::fprintf(stderr,
			"FAIL %s: offsetof=%zu expected %u*8, name=%s expected %s\n",
			field,
			actual,
			slot,
			name,
			expected_name);
		++failures;
	}
}

} // namespace

#define CHECK_JNI_OFFSET(field)                                                                   \
	checkOffset(#field,                                                                            \
		offsetof(struct JNINativeInterface, field),                                                \
		JniSlot##field,                                                                            \
		JniSlot##field < JniSlotCount ? kJniSlotNames[JniSlot##field] : "?",                       \
		#field)

#define CHECK_JVM_OFFSET(field)                                                                   \
	do {                                                                                          \
		if (offsetof(struct JNIInvokeInterface, field) !=                                         \
			static_cast<size_t>(JvmSlot##field) * sizeof(void *))                                  \
		{                                                                                         \
			std::fprintf(stderr, "FAIL JavaVM %s offset mismatch\n", #field);                     \
			++failures;                                                                           \
		}                                                                                         \
	} while (0)

int main()
{
	CHECK_JNI_OFFSET(GetVersion);
	CHECK_JNI_OFFSET(FindClass);
	CHECK_JNI_OFFSET(GetSuperclass);
	CHECK_JNI_OFFSET(IsAssignableFrom);
	CHECK_JNI_OFFSET(Throw);
	CHECK_JNI_OFFSET(ExceptionOccurred);
	CHECK_JNI_OFFSET(ExceptionCheck);
	CHECK_JNI_OFFSET(PopLocalFrame);
	CHECK_JNI_OFFSET(NewGlobalRef);
	CHECK_JNI_OFFSET(NewLocalRef);
	CHECK_JNI_OFFSET(IsSameObject);
	CHECK_JNI_OFFSET(GetMethodID);
	CHECK_JNI_OFFSET(GetStaticMethodID);
	CHECK_JNI_OFFSET(GetFieldID);
	CHECK_JNI_OFFSET(GetStaticFieldID);
	CHECK_JNI_OFFSET(CallObjectMethod);
	CHECK_JNI_OFFSET(CallStaticObjectMethod);
	CHECK_JNI_OFFSET(CallStaticIntMethod);
	CHECK_JNI_OFFSET(CallStaticVoidMethodA);
	CHECK_JNI_OFFSET(GetObjectField);
	CHECK_JNI_OFFSET(SetStaticIntField);
	CHECK_JNI_OFFSET(GetStringLength);
	CHECK_JNI_OFFSET(GetStringUTFLength);
	CHECK_JNI_OFFSET(NewStringUTF);
	CHECK_JNI_OFFSET(GetStringUTFChars);
	CHECK_JNI_OFFSET(ReleaseStringUTFChars);
	CHECK_JNI_OFFSET(RegisterNatives);
	CHECK_JNI_OFFSET(GetJavaVM);
	CHECK_JNI_OFFSET(MonitorEnter);
	CHECK_JNI_OFFSET(GetObjectRefType);

	CHECK_JVM_OFFSET(DestroyJavaVM);
	CHECK_JVM_OFFSET(AttachCurrentThread);
	CHECK_JVM_OFFSET(DetachCurrentThread);
	CHECK_JVM_OFFSET(GetEnv);
	CHECK_JVM_OFFSET(AttachCurrentThreadAsDaemon);

	// The table ends at GetObjectRefType; a newer header would change this.
	const size_t last = offsetof(struct JNINativeInterface, GetObjectRefType);
	if (last / sizeof(void *) + 1 != JniSlotCount)
	{
		std::fprintf(stderr, "FAIL JNINativeInterface has %zu entries, header expects %u\n",
			last / sizeof(void *) + 1,
			JniSlotCount);
		++failures;
	}

	if (failures == 0)
	{
		std::printf("jni_layout_test: OK\n");
		return 0;
	}
	std::printf("jni_layout_test: %d check(s) failed\n", failures);
	return 1;
}
