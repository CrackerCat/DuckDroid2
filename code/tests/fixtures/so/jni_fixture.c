// SPDX-License-Identifier: GPL-2.0-only
// Guest-side JNI fixture for the M5 tests. Built for aarch64-linux-android
// with -nostdlib; the only imports are none: every JNI call goes through the
// function table handed over by JNI_OnLoad.
#include <jni.h>
#include <stdarg.h>

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

// Object arrays: build a one-element String[] from a string the JNI table
// created, then read it back through a local reference.
static jint object_array_length(JNIEnv *env, jclass klass)
{
	(void)klass;
	jclass string_class = (*env)->FindClass(env, "java/lang/String");
	if (string_class == 0)
	{
		return -103;
	}
	jstring text = (*env)->NewStringUTF(env, "hi");
	if (text == 0)
	{
		return -104;
	}
	jobjectArray array = (*env)->NewObjectArray(env, 1, string_class, text);
	if (array == 0)
	{
		return -105;
	}
	if ((*env)->GetArrayLength(env, array) != 1)
	{
		return -106;
	}
	jobject element = (*env)->GetObjectArrayElement(env, array, 0);
	if (element == 0)
	{
		return -107;
	}
	const char *characters = (*env)->GetStringUTFChars(env, (jstring)element, 0);
	if (characters == 0)
	{
		return -108;
	}
	const jint length = (*env)->GetStringLength(env, (jstring)element);
	(*env)->ReleaseStringUTFChars(env, (jstring)element, characters);
	return length;
}

// NewObjectV reads its arguments from a caller-owned va_list, exactly as
// libjdg-style native code does. The constructor shim stores the value.
static jobject new_object_v(JNIEnv *env, jclass klass, jmethodID ctor, ...)
{
	va_list args;
	va_start(args, ctor);
	jobject object = (*env)->NewObjectV(env, klass, ctor, args);
	va_end(args);
	return object;
}

static jint new_object_value(JNIEnv *env, jclass fixture)
{
	(void)fixture;
	jclass box = (*env)->FindClass(env, "env/Box");
	if (box == 0)
	{
		return -110;
	}
	jmethodID ctor = (*env)->GetMethodID(env, box, "<init>", "(I)V");
	if (ctor == 0)
	{
		return -111;
	}
	jobject object = new_object_v(env, box, ctor, (jint)7);
	if (object == 0)
	{
		return -112;
	}
	jfieldID value = (*env)->GetFieldID(env, box, "value", "I");
	if (value == 0)
	{
		return -113;
	}
	return (*env)->GetIntField(env, object, value);
}

// CallStaticObjectMethodV: the host shim receives a jstring and a jint from a
// va_list and returns a new string.
static jobject call_static_object_v(JNIEnv *env, jclass klass, jmethodID method, ...)
{
	va_list args;
	va_start(args, method);
	jobject result = (*env)->CallStaticObjectMethodV(env, klass, method, args);
	va_end(args);
	return result;
}

static jint call_host_v(JNIEnv *env, jclass fixture)
{
	jmethodID method = (*env)->GetStaticMethodID(
		env, fixture, "hostJoin", "(Ljava/lang/String;I)Ljava/lang/String;");
	if (method == 0)
	{
		return -120;
	}
	jstring argument = (*env)->NewStringUTF(env, "hi");
	if (argument == 0)
	{
		return -121;
	}
	jobject result = call_static_object_v(env, fixture, method, argument, (jint)7);
	if (result == 0)
	{
		return -122;
	}
	return (*env)->GetStringLength(env, (jstring)result);
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
	{ "objectArrayLength", "()I", (void *)object_array_length },
	{ "newObjectValue", "()I", (void *)new_object_value },
	{ "callHostV", "()I", (void *)call_host_v },
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
