extern "C" int nps5_gpu_bridge_version(void) { return 1; }

// libSceUserService asks libSceSystemService for the console language (keyboard layout). libSceSystemService is
// not part of this image; the language 1 (English US) is the SCE default.
extern "C" int sceSystemServiceParamGetInt(int paramId, int* value) {
    if (!value) return -1;
    (void)paramId;
    *value = 1;
    return 0;
}
