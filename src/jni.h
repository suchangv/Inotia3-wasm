/*
 * jni.h -- the JNI surface the guest sees.
 *
 * This is deliberately not a copy of the NDK's <jni.h>. The guest was compiled
 * against the real one and indexes the function tables by fixed offset, so the
 * part that actually matters is the *order* of JNINativeInterface, which lives
 * in the generated jni_offsets.h. What is here is only what the host side
 * needs to read out of the guest's arguments.
 *
 * The structural difference from the Vita port is worth stating plainly,
 * because it is why lib/falso_jni cannot be vendored as-is:
 *
 *   On the Vita the guest is native ARM code sharing the loader's address
 *   space, so FalsoJNI's `JavaVM jvm` / `JNIEnv jni` -- host globals -- are
 *   valid pointers to hand the guest. Here the guest has its own address
 *   space, and a host pointer means nothing inside it.
 *
 * So the JavaVM and JNIEnv structures are materialised *in guest memory*, and
 * every slot of their function tables holds a BKPT stub address, exactly like
 * the ELF imports. Reaching a JNI function is therefore the same operation as
 * reaching libc's malloc: the interpreter decodes BKPT, derives the index from
 * the stub address, and dispatches. No new mechanism, and JNI calls show up in
 * the same crossing histogram as everything else -- which is the number this
 * spike exists to measure.
 */
#ifndef JNI_H
#define JNI_H

#include <stdint.h>

typedef int32_t  jint;
typedef int64_t  jlong;
typedef uint8_t  jboolean;
typedef void    *jobject;
typedef void    *jclass;
typedef void    *jstring;
typedef void    *jmethodID;
typedef void    *jfieldID;
typedef void    *jarray;

#define JNI_OK          0
#define JNI_ERR        (-1)
#define JNI_VERSION_1_6 0x00010006

/* Guest addresses of the two structs, valid after jni_guest_init(). These are
 * what gets handed to JNI_OnLoad and to every native* entry point. */
extern int g_jni_verbose;
extern uint32_t g_jnienv_ptr;
extern uint32_t g_javavm_ptr;

/* Build the JavaVM and JNIEnv in guest memory and register a stub for every
 * entry of both function tables. Call after elf_load() -- it uses
 * import_register() -- and before running any guest code. */
void jni_guest_init(void);

/* Create a fake array handle for the host side to pass into a guest entry
 * point -- nativePreInit() takes a jintArray out-parameter that the Java side
 * would have allocated, and nativeInit() takes jlongArray/jintArray/jfloatArray
 * in-parameters. `elem` is the element size in bytes (4 for int/float, 8 for
 * long). */
uint32_t jni_host_new_int_array(int n);
uint32_t jni_host_new_array(int elem, int n);

/* Guest address of an array handle's payload, for the host to fill in a value
 * the Java side would have supplied (e.g. the current wall-clock time). */
uint32_t jni_host_array_addr(uint32_t handle);

/* True if the named import is a JNI table entry, and if so, call it.
 * Returns 1 when it handled the call. */
int jni_host_call(const char *name, uint32_t *args, int nargs, uint32_t *ret_lo,
                  uint32_t *ret_hi);

/* Report which JNI functions the guest called, and which are still missing. */
void jni_report(int top_n);

#endif /* JNI_H */
