/*
 * Xposed enables "god mode" for developers
 */

#define LOG_TAG "Xposed"

#include "xposed.h"

#include <utils/Log.h>
#include <android_runtime/AndroidRuntime.h>

#include <stdio.h>
#include <sys/mman.h>
#include <cutils/properties.h>
#include <dlfcn.h>

#include "xposed_offsets.h"

#include "native/InternalNativePriv.h"

int RUNNING_PLATFORM_SDK_VERSION = 0;
void (*PTR_atrace_set_tracing_enabled)(bool) = NULL;

namespace android {

////////////////////////////////////////////////////////////
// variables
////////////////////////////////////////////////////////////
bool keepLoadingXposed = false;
ClassObject* objectArrayClass = NULL;
jclass xposedClass = NULL;
Method* xposedHandleHookedMethod = NULL;

void* PTR_gDvmJit = NULL;
size_t arrayContentsOffset = 0;


#if PLATFORM_SDK_VERSION < 14

#define dvmUnboxPrimitive dvmUnwrapPrimitive
#define dvmBoxPrimitive dvmWrapPrimitive

static void dvmThrowNullPointerException(const char* msg) {
    dvmThrowException("Ljava/lang/NullPointerException;", msg);
}

static void dvmThrowClassCastException(ClassObject* actual, ClassObject* desired)
{
    dvmThrowExceptionFmt("Ljava/lang/ClassCastException;",
        "%s cannot be cast to %s", actual->descriptor, desired->descriptor);
}

static void dvmThrowIllegalArgumentException(const char* msg) {
    dvmThrowException("Ljava/lang/IllegalArgumentException;", msg);
}

static void dvmThrowNoSuchMethodError(const char* msg) {
    dvmThrowException("Ljava/lang/NoSuchMethodError;", msg);
}

static Object* dvmDecodeIndirectRef(::Thread* self, jobject jobj) {
    if (jobj == NULL) {
        return NULL;
    }
    return dvmDecodeIndirectRef(self->jniEnv, jobj);
}
#endif


////////////////////////////////////////////////////////////
// called directoy by JNI_OnLoad
////////////////////////////////////////////////////////////
void initTypePointers()
{
    char sdk[PROPERTY_VALUE_MAX];
    const char *error;

    property_get("ro.build.version.sdk", sdk, "0");
    RUNNING_PLATFORM_SDK_VERSION = atoi(sdk);

    dlerror();

    if (RUNNING_PLATFORM_SDK_VERSION >= 18) {
        *(void **) (&PTR_atrace_set_tracing_enabled) = dlsym(RTLD_DEFAULT, "atrace_set_tracing_enabled");
        if ((error = dlerror()) != NULL) {
            ALOGE("Could not find address for function atrace_set_tracing_enabled: %s", error);
        }
    }
}

void xposedInfo() {
    char release[PROPERTY_VALUE_MAX];
    char sdk[PROPERTY_VALUE_MAX];
    char manufacturer[PROPERTY_VALUE_MAX];
    char model[PROPERTY_VALUE_MAX];
    char rom[PROPERTY_VALUE_MAX];
    char fingerprint[PROPERTY_VALUE_MAX];

    
    property_get("ro.build.version.release", release, "n/a");
    property_get("ro.build.version.sdk", sdk, "n/a");
    property_get("ro.product.manufacturer", manufacturer, "n/a");
    property_get("ro.product.model", model, "n/a");
    property_get("ro.build.display.id", rom, "n/a");
    property_get("ro.build.fingerprint", fingerprint, "n/a");

    
    LOGI("Starting Dexposed binary version %s, compiled for SDK %d\n", XPOSED_VERSION, PLATFORM_SDK_VERSION);
    ALOGD("Phone: %s (%s), Android version %s (SDK %s)\n", model, manufacturer, release, sdk);
    ALOGD("ROM: %s\n", rom);
    ALOGD("Build fingerprint: %s\n", fingerprint);
}

bool isRunningDalvik() {
    if (RUNNING_PLATFORM_SDK_VERSION < 19)
        return true;

    char runtime[PROPERTY_VALUE_MAX];
    property_get("persist.sys.dalvik.vm.lib", runtime, "");
    if (strcmp(runtime, "libdvm.so") != 0) {
        ALOGE("Unsupported runtime library %s, setting to libdvm.so", runtime);
        return false;
    } else {
    	return true;
    }
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv* env = NULL;
    jint result = -1;

    if (vm->GetEnv((void**) &env, JNI_VERSION_1_6) != JNI_OK) {
        return result;
    }

    initTypePointers();
    xposedInfo();
    keepLoadingXposed = isRunningDalvik();
    keepLoadingXposed = xposedOnVmCreated(env, NULL);

    return JNI_VERSION_1_6;
}

bool xposedOnVmCreated(JNIEnv* env, const char* className) {

    keepLoadingXposed = keepLoadingXposed && xposedInitMemberOffsets(env);
    if (!keepLoadingXposed)
        return false;

    // disable some access checks
    patchReturnTrue((uintptr_t) &dvmCheckClassAccess);
    patchReturnTrue((uintptr_t) &dvmCheckFieldAccess);
    patchReturnTrue((uintptr_t) &dvmInSamePackage);
    patchReturnTrue((uintptr_t) &dvmCheckMethodAccess);

    env->ExceptionClear();

    xposedClass = env->FindClass(XPOSED_CLASS);
    xposedClass = reinterpret_cast<jclass>(env->NewGlobalRef(xposedClass));
    
    if (xposedClass == NULL) {
        ALOGE("Error while loading Xposed class '%s':\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    ALOGI("Found Xposed class '%s', now initializing\n", XPOSED_CLASS);
    if (register_com_taobao_android_dexposed_XposedBridge(env) != JNI_OK) {
        ALOGE("Could not register natives for '%s'\n", XPOSED_CLASS);
        return false;
    }


    jmethodID xposedbridgeMainMethod = env->GetStaticMethodID(xposedClass, "main","()V");
    if (xposedbridgeMainMethod == NULL) {
		ALOGE("ERROR: could not find method %s.main()\n", XPOSED_CLASS);
		dvmLogExceptionStackTrace();
		env->ExceptionClear();
		keepLoadingXposed = false;
		return false;
    }
    env->CallStaticVoidMethod(xposedClass, xposedbridgeMainMethod);

    return true;
}


static bool xposedInitMemberOffsets(JNIEnv* env) {

    PTR_gDvmJit = dlsym(RTLD_DEFAULT, "gDvmJit");

    if (PTR_gDvmJit == NULL) {
        offsetMode = MEMBER_OFFSET_MODE_NO_JIT;
    } else {
        offsetMode = MEMBER_OFFSET_MODE_WITH_JIT;
    }
    ALOGD("Using structure member offsets for mode %s", xposedOffsetModesDesc[offsetMode]);

    MEMBER_OFFSET_COPY(DvmJitGlobals, codeCacheFull);

    // detect offset of ArrayObject->contents
    jintArray dummyArray = env->NewIntArray(1);
    if (dummyArray == NULL) {
        LOGE("Could allocate int array for testing");
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    jint* dummyArrayElements = env->GetIntArrayElements(dummyArray, NULL);
    arrayContentsOffset = (size_t)dummyArrayElements - (size_t)dvmDecodeIndirectRef(dvmThreadSelf(), dummyArray);
    env->ReleaseIntArrayElements(dummyArray,dummyArrayElements, 0);
    env->DeleteLocalRef(dummyArray);

    if (arrayContentsOffset < 12 || arrayContentsOffset > 128) {
        LOGE("Detected strange offset %d of ArrayObject->contents", arrayContentsOffset);
        return false;
    }
    return true;
}

static inline void xposedSetObjectArrayElement(const ArrayObject* obj, int index, Object* val) {
    uintptr_t arrayContents = (uintptr_t)obj + arrayContentsOffset;
    ((Object **)arrayContents)[index] = val;
    dvmWriteBarrierArray(obj, index, index + 1);
}


////////////////////////////////////////////////////////////
// handling hooked methods / helpers
////////////////////////////////////////////////////////////

static void xposedCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self) {

    if (!xposedIsHooked(method)) {
        dvmThrowNoSuchMethodError("could not find Xposed original method - how did you even get here?");
        return;
    }

    XposedHookInfo* hookInfo = (XposedHookInfo*) method->insns;
    Method* original = (Method*) hookInfo;
    Object* originalReflected = hookInfo->reflectedMethod;
    Object* additionalInfo = hookInfo->additionalInfo;
  
    // convert/box arguments
    const char* desc = &method->shorty[1]; // [0] is the return type.
    Object* thisObject = NULL;
    size_t srcIndex = 0;
    size_t dstIndex = 0;
    
    // for non-static methods determine the "this" pointer
    if (!dvmIsStaticMethod(original)) {
        thisObject = (Object*) args[0];
        srcIndex++;
    }
    
    ArrayObject* argsArray = dvmAllocArrayByClass(objectArrayClass, strlen(method->shorty) - 1, ALLOC_DEFAULT);
    if (argsArray == NULL) {
        return;
    }
    
    while (*desc != '\0') {
        char descChar = *(desc++);
        JValue value;
        Object* obj;

        switch (descChar) {
        case 'Z':
        case 'C':
        case 'F':
        case 'B':
        case 'S':
        case 'I':
            value.i = args[srcIndex++];
            obj = (Object*) dvmBoxPrimitive(value, dvmFindPrimitiveClass(descChar));
            dvmReleaseTrackedAlloc(obj, self);
            break;
        case 'D':
        case 'J':
            value.j = dvmGetArgLong(args, srcIndex);
            srcIndex += 2;
            obj = (Object*) dvmBoxPrimitive(value, dvmFindPrimitiveClass(descChar));
            dvmReleaseTrackedAlloc(obj, self);
            break;
        case '[':
        case 'L':
            obj  = (Object*) args[srcIndex++];
            break;
        default:
            ALOGE("Unknown method signature description character: %c\n", descChar);
            obj = NULL;
            srcIndex++;
        }
        xposedSetObjectArrayElement(argsArray, dstIndex++, obj);
    }
    
    // call the Java handler function
    JValue result;
    dvmCallMethod(self, xposedHandleHookedMethod, NULL, &result,
        originalReflected, (int) original, additionalInfo, thisObject, argsArray);
        
    dvmReleaseTrackedAlloc((Object *)argsArray, self);

    // exceptions are thrown to the caller
    if (dvmCheckException(self)) {
        return;
    }

    // return result with proper type
    ClassObject* returnType = dvmGetBoxedReturnType(method);
    if (returnType->primitiveType == PRIM_VOID) {
        // ignored
    } else if (result.l == NULL) {
        if (dvmIsPrimitiveClass(returnType)) {
            dvmThrowNullPointerException("null result when primitive expected");
        }
        pResult->l = NULL;
    } else {
        if (!dvmUnboxPrimitive((Object *)result.l, returnType, pResult)) {
            dvmThrowClassCastException(((Object *)result.l)->clazz, returnType);

        }
    }
}


static void replaceAsm(uintptr_t function, unsigned const char* newCode, size_t len) {
#ifdef __arm__
    function = function & ~1;
#endif
    uintptr_t pageStart = function & ~(PAGESIZE-1);
    size_t pageProtectSize = PAGESIZE;
    if (function+len > pageStart+pageProtectSize)
        pageProtectSize += PAGESIZE;

    mprotect((void*)pageStart, pageProtectSize, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy((void*)function, newCode, len);
    mprotect((void*)pageStart, pageProtectSize, PROT_READ | PROT_EXEC);

    __clear_cache((void*)function, (void*)(function+len));
}

static void patchReturnTrue(uintptr_t function) {
#ifdef __arm__
    unsigned const char asmReturnTrueThumb[] = { 0x01, 0x20, 0x70, 0x47 };
    unsigned const char asmReturnTrueArm[] = { 0x01, 0x00, 0xA0, 0xE3, 0x1E, 0xFF, 0x2F, 0xE1 };
    if (function & 1)
        replaceAsm(function, asmReturnTrueThumb, sizeof(asmReturnTrueThumb));
    else
        replaceAsm(function, asmReturnTrueArm, sizeof(asmReturnTrueArm));
#else
    unsigned const char asmReturnTrueX86[] = { 0x31, 0xC0, 0x40, 0xC3 };
    replaceAsm(function, asmReturnTrueX86, sizeof(asmReturnTrueX86));
#endif
}



////////////////////////////////////////////////////////////
// JNI methods
////////////////////////////////////////////////////////////

static jboolean com_taobao_android_dexposed_XposedBridge_initNative(JNIEnv* env, jclass clazz) {

	if (!keepLoadingXposed) {
        ALOGE("Not initializing Xposed because of previous errors\n");
        return false;
    }

    ::Thread* self = dvmThreadSelf();

    xposedHandleHookedMethod = (Method*) env->GetStaticMethodID(xposedClass, "handleHookedMethod",
        "(Ljava/lang/reflect/Member;ILjava/lang/Object;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (xposedHandleHookedMethod == NULL) {
        LOGE("ERROR: could not find method %s.handleHookedMethod(Member, int, Object, Object, Object[])\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        keepLoadingXposed = false;
        return false;
    }

    Method* xposedInvokeOriginalMethodNative = (Method*) env->GetStaticMethodID(xposedClass, "invokeOriginalMethodNative",
        "(Ljava/lang/reflect/Member;I[Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (xposedInvokeOriginalMethodNative == NULL) {
        LOGE("ERROR: could not find method %s.invokeOriginalMethodNative(Member, int, Class[], Class, Object, Object[])\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        keepLoadingXposed = false;
        return false;
    }
    dvmSetNativeFunc(xposedInvokeOriginalMethodNative, com_taobao_android_dexposed_XposedBridge_invokeOriginalMethodNative, NULL);

    Method* xposedInvokeSuperNative = (Method*) env->GetStaticMethodID(xposedClass, "invokeSuperNative",
            "(Ljava/lang/Object;[Ljava/lang/Object;Ljava/lang/reflect/Member;Ljava/lang/Class;[Ljava/lang/Class;Ljava/lang/Class;I)Ljava/lang/Object;");
	if (xposedInvokeSuperNative == NULL) {
		LOGE("ERROR: could not find method %s.xposedInvokeNonVirtual(Object, Object[], Class, Class[], Class, int, boolean)\n", XPOSED_CLASS);
		dvmLogExceptionStackTrace();
		env->ExceptionClear();
		keepLoadingXposed = false;
		return false;
	}
    dvmSetNativeFunc(xposedInvokeSuperNative, com_taobao_android_dexposed_XposedBridge_invokeSuperNative, NULL);

    objectArrayClass = dvmFindArrayClass("[Ljava/lang/Object;", NULL);
    if (objectArrayClass == NULL) {
        LOGE("Error while loading Object[] class");
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        keepLoadingXposed = false;
        return false;
    }

    return true;
}

static void com_taobao_android_dexposed_XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethodIndirect,
            jobject declaredClassIndirect, jint slot, jobject additionalInfoIndirect) {
    // Usage errors?
    if (declaredClassIndirect == NULL || reflectedMethodIndirect == NULL) {
        dvmThrowIllegalArgumentException("method and declaredClass must not be null");
        return;
    }
    
    // Find the internal representation of the method
    ClassObject* declaredClass = (ClassObject*) dvmDecodeIndirectRef(dvmThreadSelf(), declaredClassIndirect);
    Method* method = dvmSlotToMethod(declaredClass, slot);
    if (method == NULL) {
        dvmThrowNoSuchMethodError("could not get internal representation for method");
        return;
    }
    
    if (xposedIsHooked(method)) {
        // already hooked
        return;
    }
    
    // Save a copy of the original method and other hook info
    XposedHookInfo* hookInfo = (XposedHookInfo*) calloc(1, sizeof(XposedHookInfo));
    memcpy(hookInfo, method, sizeof(hookInfo->originalMethodStruct));
    hookInfo->reflectedMethod = dvmDecodeIndirectRef(dvmThreadSelf(), env->NewGlobalRef(reflectedMethodIndirect));
    hookInfo->additionalInfo = dvmDecodeIndirectRef(dvmThreadSelf(), env->NewGlobalRef(additionalInfoIndirect));

    // Replace method with our own code
    SET_METHOD_FLAG(method, ACC_NATIVE);
    method->nativeFunc = &xposedCallHandler;
    method->insns = (const u2*) hookInfo;
    method->registersSize = method->insSize;
    method->outsSize = 0;

    if (PTR_gDvmJit != NULL) {
        // reset JIT cache
        MEMBER_VAL(PTR_gDvmJit, DvmJitGlobals, codeCacheFull) = true;
    }
}

/*
* private Object invokeSuperNative(Object obj, Object[] args, Member method, Class declaringClass,
*   Class[] parameterTypes, Class returnType, int slot)
*
* Invoke a super method via reflection.
*/
static void com_taobao_android_dexposed_XposedBridge_invokeSuperNative(const u4* args,
    JValue* pResult, const Method* method, ::Thread* self)
{
    Object* methObj = (Object*) args[0];
    ArrayObject* argList = (ArrayObject*) args[1];
    Method* thisMethod = (Method*)args[2];
    ClassObject* declaringClass = (ClassObject*) args[3];
    ArrayObject* params = (ArrayObject*) args[4];
    ClassObject* returnType = (ClassObject*) args[5];
    int slot = args[6];
    const Method* meth;
    Object* result;

    if(methObj == NULL)   // static methods didn't has super
    	RETURN_VOID();

    declaringClass = declaringClass->super; //get the super class
    if(declaringClass == NULL)
    	RETURN_VOID();

    /*
     * "If the underlying method is static, the class that declared the
     * method is initialized if it has not already been initialized."
     */
    meth = dvmSlotToMethod(declaringClass, slot);
    assert(meth != NULL);
    if (dvmIsStaticMethod(meth)) {
        if (!dvmIsClassInitialized(declaringClass)) {
            if (!dvmInitClass(declaringClass))
                goto init_failed;
        }
    } else {
        /* looks like interfaces need this too? */
        if (dvmIsInterfaceClass(declaringClass) &&
            !dvmIsClassInitialized(declaringClass))
        {
            if (!dvmInitClass(declaringClass))
                goto init_failed;
        }

#if PLATFORM_SDK_VERSION > 13
        /* make sure the object is an instance of the expected class */
        if (!dvmVerifyObjectInClass(methObj, declaringClass)) {
            assert(dvmCheckException(dvmThreadSelf()));
            RETURN_VOID();
        }
#endif

    }

    /*
     * If the method has a return value, "result" will be an object or
     * a boxed primitive.
     */
    result = dvmInvokeMethod(methObj, meth, argList, params, returnType, true);

    RETURN_PTR(result);

init_failed:
    /*
     * If initialization failed, an exception will be raised.
     */
    ALOGD("Method.invoke() on bad class %s failed",
        declaringClass->descriptor);
    assert(dvmCheckException(dvmThreadSelf()));
    RETURN_VOID();
}



static inline bool xposedIsHooked(const Method* method) {
    return (method->nativeFunc == &xposedCallHandler);
}

// simplified copy of Method.invokeNative, but calls the original (non-hooked) method and has no access checks
// used when a method has been hooked
static void com_taobao_android_dexposed_XposedBridge_invokeOriginalMethodNative(const u4* args, JValue* pResult,
            const Method* method, ::Thread* self) {
    Method* meth = (Method*) args[1];
    if (meth == NULL) {
        meth = dvmGetMethodFromReflectObj((Object*) args[0]);
        if (xposedIsHooked(meth)) {
            meth = (Method*) meth->insns;
        }
    }
    ArrayObject* params = (ArrayObject*) args[2];
    ClassObject* returnType = (ClassObject*) args[3];
    Object* thisObject = (Object*) args[4]; // null for static methods
    ArrayObject* argList = (ArrayObject*) args[5];

    // invoke the method
    pResult->l = dvmInvokeMethod(thisObject, meth, argList, params, returnType, true);
    return;
}

static const JNINativeMethod xposedMethods[] = {
    {"hookMethodNative", "(Ljava/lang/reflect/Member;Ljava/lang/Class;ILjava/lang/Object;)V", (void*)com_taobao_android_dexposed_XposedBridge_hookMethodNative},
    {"initNative", "()Z", (void*)com_taobao_android_dexposed_XposedBridge_initNative},
};

static int register_com_taobao_android_dexposed_XposedBridge(JNIEnv* env) {
    return env->RegisterNatives(xposedClass, xposedMethods, NELEM(xposedMethods));
}

}

