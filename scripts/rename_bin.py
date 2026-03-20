Import("env")
import shutil, os

def copy_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    bin_src = os.path.join(build_dir, "firmware.bin")
    releases_dir = os.path.join(project_dir, "releases")
    os.makedirs(releases_dir, exist_ok=True)
    bin_dst = os.path.join(releases_dir, "herald-firmware.bin")
    if os.path.exists(bin_src):
        shutil.copy(bin_src, bin_dst)
        print(f"Copied firmware to {bin_dst}")

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_bin)
