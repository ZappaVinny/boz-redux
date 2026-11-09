// The launcher's operating system helpers on Android: no child processes and no desktop file
// dialogs, so the system parts go through static methods of the launcher's Java activity
// (packaging/android/.../LauncherActivity.java). SDL attaches any thread that asks for JNI.
#include "os.h"

#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>

namespace os {

namespace {

// One JNI call's environment and the activity's class.
struct Java {
    JNIEnv *env = nullptr;
    jclass cls = nullptr;

    Java() {
        env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
        jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (env && activity) {
            // FindClass on a worker thread only sees system classes: ask the activity instead.
            cls = env->GetObjectClass(activity);
            env->DeleteLocalRef(activity);
        }
    }
    ~Java() {
        if (cls) {
            env->DeleteLocalRef(cls);
        }
    }
    bool ok() const { return env && cls; }

    jmethodID method(const char *name, const char *signature) {
        jmethodID id = env->GetStaticMethodID(cls, name, signature);
        if (!id) {
            env->ExceptionClear();
        }
        return id;
    }
    jstring string(const std::string &text) { return env->NewStringUTF(text.c_str()); }
    std::string take(jstring text) {
        if (!text) {
            return "";
        }
        const char *chars = env->GetStringUTFChars(text, nullptr);
        std::string out = chars ? chars : "";
        env->ReleaseStringUTFChars(text, chars);
        env->DeleteLocalRef(text);
        return out;
    }
    jobjectArray strings(const std::vector<std::string> &items) {
        jclass string_class = env->FindClass("java/lang/String");
        jobjectArray array = env->NewObjectArray((jsize)items.size(), string_class, nullptr);
        for (size_t i = 0; i < items.size(); ++i) {
            jstring item = string(items[i]);
            env->SetObjectArrayElement(array, (jsize)i, item);
            env->DeleteLocalRef(item);
        }
        env->DeleteLocalRef(string_class);
        return array;
    }
    // A Java exception becomes a failed call.
    bool failed() {
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return true;
        }
        return false;
    }
};

}  // namespace

std::string exe_dir() {
    return ".";
}

bool spawn(Process &, const std::string &, const std::vector<std::string> &,
           const std::map<std::string, std::string> &, const std::string &, const std::string &) {
    return false;
}

bool Process::running() {
    return false;
}

int wait(Process &process) {
    return process.exit_code;
}

bool has_file_dialogs() {
    static int available = -1;
    if (available < 0) {
        Java java;
        jmethodID id = java.ok() ? java.method("hasDocumentPicker", "()Z") : nullptr;
        available = id && java.env->CallStaticBooleanMethod(java.cls, id) && !java.failed();
    }
    return available;
}

std::vector<std::string> pick_documents(bool multiple) {
    std::vector<std::string> picked;
    Java java;
    jmethodID id = java.ok() ? java.method("pickDocuments", "(Z)[Ljava/lang/String;") : nullptr;
    if (!id) {
        return picked;
    }
    auto array = (jobjectArray)java.env->CallStaticObjectMethod(java.cls, id, (jboolean)multiple);
    if (java.failed() || !array) {
        return picked;
    }
    jsize count = java.env->GetArrayLength(array);
    for (jsize i = 0; i < count; ++i) {
        std::string uri = java.take((jstring)java.env->GetObjectArrayElement(array, i));
        if (!uri.empty()) {
            picked.push_back(uri);
        }
    }
    java.env->DeleteLocalRef(array);
    return picked;
}

std::string pick_file(const char *, const char *, const char *) {
    std::vector<std::string> picked = pick_documents(false);
    return picked.empty() ? "" : picked[0];
}

std::string pick_folder(const char *) {
    return "";
}

void open_path(const std::string &) {}

int64_t document_size(const std::string &uri) {
    Java java;
    jmethodID id = java.ok() ? java.method("documentSize", "(Ljava/lang/String;)J") : nullptr;
    if (!id) {
        return -1;
    }
    jstring juri = java.string(uri);
    jlong size = java.env->CallStaticLongMethod(java.cls, id, juri);
    java.env->DeleteLocalRef(juri);
    return java.failed() ? -1 : (int64_t)size;
}

bool copy_document(const std::string &uri, const std::string &to) {
    Java java;
    jmethodID id = java.ok() ? java.method("copyDocument", "(Ljava/lang/String;Ljava/lang/String;)Z")
                             : nullptr;
    if (!id) {
        return false;
    }
    jstring juri = java.string(uri), jto = java.string(to);
    bool ok = java.env->CallStaticBooleanMethod(java.cls, id, juri, jto);
    java.env->DeleteLocalRef(juri);
    java.env->DeleteLocalRef(jto);
    return !java.failed() && ok;
}

std::string import_mod_zip(const std::string &uri, const std::string &mods_dir, std::string *error) {
    Java java;
    jmethodID id = java.ok() ? java.method("importModZip",
                                           "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
                             : nullptr;
    if (!id) {
        *error = "The launcher's Java side is missing importModZip.";
        return "";
    }
    jstring juri = java.string(uri), jdir = java.string(mods_dir);
    // "ok:<folder>" or "error:<reason>"
    std::string result = java.take((jstring)java.env->CallStaticObjectMethod(java.cls, id, juri, jdir));
    java.env->DeleteLocalRef(juri);
    java.env->DeleteLocalRef(jdir);
    if (java.failed() || result.compare(0, 3, "ok:") != 0) {
        *error = result.compare(0, 6, "error:") == 0 ? result.substr(6) : "That file could not be read.";
        return "";
    }
    return result.substr(3);
}

bool download(const std::string &url, const std::string &to) {
    Java java;
    jmethodID id =
        java.ok() ? java.method("download", "(Ljava/lang/String;Ljava/lang/String;)Z") : nullptr;
    if (!id) {
        return false;
    }
    jstring jurl = java.string(url), jto = java.string(to);
    bool ok = java.env->CallStaticBooleanMethod(java.cls, id, jurl, jto);
    java.env->DeleteLocalRef(jurl);
    java.env->DeleteLocalRef(jto);
    return !java.failed() && ok;
}

bool start_game(const std::vector<std::string> &args, const std::map<std::string, std::string> &env) {
    Java java;
    jmethodID id = java.ok() ? java.method("startGame", "([Ljava/lang/String;[Ljava/lang/String;)Z")
                             : nullptr;
    if (!id) {
        return false;
    }
    std::vector<std::string> pairs;
    for (const auto &entry : env) {
        pairs.push_back(entry.first);
        pairs.push_back(entry.second);
    }
    jobjectArray jargs = java.strings(args), jenv = java.strings(pairs);
    bool ok = java.env->CallStaticBooleanMethod(java.cls, id, jargs, jenv);
    java.env->DeleteLocalRef(jargs);
    java.env->DeleteLocalRef(jenv);
    return !java.failed() && ok;
}

}  // namespace os
