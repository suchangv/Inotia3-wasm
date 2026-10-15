/*
 * jni_offsets.h -- generated from lib/falso_jni/jni.h; do not edit by hand.
 *
 * The order, and the number of reserved slots ahead of it, are the ABI. The
 * guest reaches every JNI function by indexing one of these tables at a fixed
 * byte offset, so an entry one slot out of place is not a cosmetic error -- it
 * silently calls the neighbouring function, and the only symptom is a wrong
 * return value much later.
 *
 * The two tables do not have the same number of reserved slots
 * (JNINativeInterface has 4, JNIInvokeInterface has 3), which is exactly the
 * kind of asymmetry that is easy to assume away. It was: assuming 4 for both
 * shifted the whole JavaVM table by one slot, and `GetEnv` -- recognisable by
 * its JNI_VERSION_1_6 argument -- arrived as `DetachCurrentThread`.
 *
 * Entry i of a table therefore lives at byte offset (i + reserved) * 4.
 */
#ifndef JNI_OFFSETS_H
#define JNI_OFFSETS_H

#define JNI_ENV_RESERVED 4
#define JNI_ENV_COUNT    229
#define JNI_VM_RESERVED  3
#define JNI_VM_COUNT     5

static const char *const k_jni_env_names[JNI_ENV_COUNT] = {
    "GetVersion", "DefineClass", "FindClass", "FromReflectedMethod",
    "FromReflectedField", "ToReflectedMethod", "GetSuperclass", "IsAssignableFrom",
    "ToReflectedField", "Throw", "ThrowNew", "ExceptionOccurred",
    "ExceptionDescribe", "ExceptionClear", "FatalError", "PushLocalFrame",
    "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef",
    "IsSameObject", "NewLocalRef", "EnsureLocalCapacity", "AllocObject",
    "NewObject", "NewObjectV", "NewObjectA", "GetObjectClass",
    "IsInstanceOf", "GetMethodID", "CallObjectMethod", "CallObjectMethodV",
    "CallObjectMethodA", "CallBooleanMethod", "CallBooleanMethodV", "CallBooleanMethodA",
    "CallByteMethod", "CallByteMethodV", "CallByteMethodA", "CallCharMethod",
    "CallCharMethodV", "CallCharMethodA", "CallShortMethod", "CallShortMethodV",
    "CallShortMethodA", "CallIntMethod", "CallIntMethodV", "CallIntMethodA",
    "CallLongMethod", "CallLongMethodV", "CallLongMethodA", "CallFloatMethod",
    "CallFloatMethodV", "CallFloatMethodA", "CallDoubleMethod", "CallDoubleMethodV",
    "CallDoubleMethodA", "CallVoidMethod", "CallVoidMethodV", "CallVoidMethodA",
    "CallNonvirtualObjectMethod", "CallNonvirtualObjectMethodV", "CallNonvirtualObjectMethodA", "CallNonvirtualBooleanMethod",
    "CallNonvirtualBooleanMethodV", "CallNonvirtualBooleanMethodA", "CallNonvirtualByteMethod", "CallNonvirtualByteMethodV",
    "CallNonvirtualByteMethodA", "CallNonvirtualCharMethod", "CallNonvirtualCharMethodV", "CallNonvirtualCharMethodA",
    "CallNonvirtualShortMethod", "CallNonvirtualShortMethodV", "CallNonvirtualShortMethodA", "CallNonvirtualIntMethod",
    "CallNonvirtualIntMethodV", "CallNonvirtualIntMethodA", "CallNonvirtualLongMethod", "CallNonvirtualLongMethodV",
    "CallNonvirtualLongMethodA", "CallNonvirtualFloatMethod", "CallNonvirtualFloatMethodV", "CallNonvirtualFloatMethodA",
    "CallNonvirtualDoubleMethod", "CallNonvirtualDoubleMethodV", "CallNonvirtualDoubleMethodA", "CallNonvirtualVoidMethod",
    "CallNonvirtualVoidMethodV", "CallNonvirtualVoidMethodA", "GetFieldID", "GetObjectField",
    "GetBooleanField", "GetByteField", "GetCharField", "GetShortField",
    "GetIntField", "GetLongField", "GetFloatField", "GetDoubleField",
    "SetObjectField", "SetBooleanField", "SetByteField", "SetCharField",
    "SetShortField", "SetIntField", "SetLongField", "SetFloatField",
    "SetDoubleField", "GetStaticMethodID", "CallStaticObjectMethod", "CallStaticObjectMethodV",
    "CallStaticObjectMethodA", "CallStaticBooleanMethod", "CallStaticBooleanMethodV", "CallStaticBooleanMethodA",
    "CallStaticByteMethod", "CallStaticByteMethodV", "CallStaticByteMethodA", "CallStaticCharMethod",
    "CallStaticCharMethodV", "CallStaticCharMethodA", "CallStaticShortMethod", "CallStaticShortMethodV",
    "CallStaticShortMethodA", "CallStaticIntMethod", "CallStaticIntMethodV", "CallStaticIntMethodA",
    "CallStaticLongMethod", "CallStaticLongMethodV", "CallStaticLongMethodA", "CallStaticFloatMethod",
    "CallStaticFloatMethodV", "CallStaticFloatMethodA", "CallStaticDoubleMethod", "CallStaticDoubleMethodV",
    "CallStaticDoubleMethodA", "CallStaticVoidMethod", "CallStaticVoidMethodV", "CallStaticVoidMethodA",
    "GetStaticFieldID", "GetStaticObjectField", "GetStaticBooleanField", "GetStaticByteField",
    "GetStaticCharField", "GetStaticShortField", "GetStaticIntField", "GetStaticLongField",
    "GetStaticFloatField", "GetStaticDoubleField", "SetStaticObjectField", "SetStaticBooleanField",
    "SetStaticByteField", "SetStaticCharField", "SetStaticShortField", "SetStaticIntField",
    "SetStaticLongField", "SetStaticFloatField", "SetStaticDoubleField", "NewString",
    "GetStringLength", "GetStringChars", "ReleaseStringChars", "NewStringUTF",
    "GetStringUTFLength", "GetStringUTFChars", "ReleaseStringUTFChars", "GetArrayLength",
    "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement", "NewBooleanArray",
    "NewByteArray", "NewCharArray", "NewShortArray", "NewIntArray",
    "NewLongArray", "NewFloatArray", "NewDoubleArray", "GetBooleanArrayElements",
    "GetByteArrayElements", "GetCharArrayElements", "GetShortArrayElements", "GetIntArrayElements",
    "GetLongArrayElements", "GetFloatArrayElements", "GetDoubleArrayElements", "ReleaseBooleanArrayElements",
    "ReleaseByteArrayElements", "ReleaseCharArrayElements", "ReleaseShortArrayElements", "ReleaseIntArrayElements",
    "ReleaseLongArrayElements", "ReleaseFloatArrayElements", "ReleaseDoubleArrayElements", "GetBooleanArrayRegion",
    "GetByteArrayRegion", "GetCharArrayRegion", "GetShortArrayRegion", "GetIntArrayRegion",
    "GetLongArrayRegion", "GetFloatArrayRegion", "GetDoubleArrayRegion", "SetBooleanArrayRegion",
    "SetByteArrayRegion", "SetCharArrayRegion", "SetShortArrayRegion", "SetIntArrayRegion",
    "SetLongArrayRegion", "SetFloatArrayRegion", "SetDoubleArrayRegion", "RegisterNatives",
    "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM",
    "GetStringRegion", "GetStringUTFRegion", "GetPrimitiveArrayCritical", "ReleasePrimitiveArrayCritical",
    "GetStringCritical", "ReleaseStringCritical", "NewWeakGlobalRef", "DeleteWeakGlobalRef",
    "ExceptionCheck", "NewDirectByteBuffer", "GetDirectBufferAddress", "GetDirectBufferCapacity",
    "GetObjectRefType",
};

static const char *const k_jni_vm_names[JNI_VM_COUNT] = {
    "DestroyJavaVM", "AttachCurrentThread", "DetachCurrentThread", "GetEnv", "AttachCurrentThreadAsDaemon",
};

#endif /* JNI_OFFSETS_H */
