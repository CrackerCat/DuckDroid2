// Guest-side JNI fixture for the M5 tests. Built for aarch64-linux-android
// with -nostdlib; the only imports are none: every JNI call goes through the
// function table handed over by JNI_OnLoad.
#include <jni.h>

#define JNI_FIXTURE_EXPORT __attribute__((visibility("default")))

static jint native_add(JNIEnv *env, jclass klass, jint a, jint b)
{
	(void)env;
	(void)klass;
	return a + b;
}

static jint call_host(JNIEnv *env, jclass klass)
{
	jmethodID method = (*env)->GetStaticMethodID(env, klass, "hostValue", "()I");
	if (method == 0)
	{
		return -100;
	}
	return (*env)->CallStaticIntMethod(env, klass, method);
}

static jint call_native(JNIEnv *env, jclass klass)
{
	jmethodID method = (*env)->GetStaticMethodID(env, klass, "nativeAdd", "(II)I");
	if (method == 0)
	{
		return -101;
	}
	return (*env)->CallStaticIntMethod(env, klass, method, 20, 22);
}

static jint find_missing(JNIEnv *env, jclass klass)
{
	(void)klass;
	jclass missing = (*env)->FindClass(env, "env/NotDeclared");
	return missing == 0 ? 1 : 0;
}

static jint find_missing_method(JNIEnv *env, jclass klass)
{
	jmethodID method = (*env)->GetStaticMethodID(env, klass, "hostValue", "()J");
	(void)method;
	return method == 0 ? 1 : 0;
}

static jint find_declared(JNIEnv *env, jclass klass)
{
	(void)klass;
	jclass declared = (*env)->FindClass(env, "env/Fixture");
	return declared != 0 ? 1 : 0;
}

static jstring make_string(JNIEnv *env, jclass klass)
{
	(void)klass;
	return (*env)->NewStringUTF(env, "value:42");
}

static jint string_length(JNIEnv *env, jclass klass)
{
	(void)klass;
	jstring text = (*env)->NewStringUTF(env, "hello");
	if (text == 0)
	{
		return -102;
	}
	return (*env)->GetStringLength(env, text);
}

// File scope so the array is initialized at load time; a local array would
// make the compiler emit a memcpy import.
static const JNINativeMethod kMethods[] = {
	{ "nativeAdd", "(II)I", (void *)native_add },
	{ "callHost", "()I", (void *)call_host },
	{ "callNative", "()I", (void *)call_native },
	{ "findMissing", "()I", (void *)find_missing },
	{ "findMissingMethod", "()I", (void *)find_missing_method },
	{ "findDeclared", "()I", (void *)find_declared },
	{ "makeString", "()Ljava/lang/String;", (void *)make_string },
	{ "stringLength", "()I", (void *)string_length },
};

JNI_FIXTURE_EXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)reserved;
	JNIEnv *env = 0;
	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
	{
		return -1;
	}
	jclass klass = (*env)->FindClass(env, "env/Fixture");
	if (klass == 0)
	{
		return -2;
	}
	const jint count = (jint)(sizeof(kMethods) / sizeof(kMethods[0]));
	if ((*env)->RegisterNatives(env, klass, kMethods, count) != JNI_OK)
	{
		return -3;
	}
	return JNI_VERSION_1_6;
}
