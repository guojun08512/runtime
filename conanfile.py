from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.scm import Git, Version
from conan.tools.files import copy, replace_in_file, chdir, save
import os

class RuntimeServiceConan(ConanFile):
    name = "runtime-service"
    description = "Runtime service for conan 2.0"
    license = "MIT"
    url = "https://github.com/guojun08512/runtime.git"
    
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "no_main": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "no_main": False,
    }

    def set_version(self):
        git = Git(self)
        try:
            # 获取最近的 tag，如果没有 tag 会抛出异常
            tag = git.run("describe --tags --abbrev=0").strip()
            # 过滤掉常见的 'v' 前缀
            self.version = tag.lstrip('v')
        except:
            # 回退逻辑：生成 0.0.0-branch.commit 格式
            try:
                branch = git.get_branch().replace("/", "_")
                commit = git.get_commit()[:7]
                self.version = f"0.0.0-{branch}.{commit}"
            except:
                self.version = "0.1.0"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        """定义标准的 CMake 布局 (生成 build/Release 等目录)"""
        cmake_layout(self)

    def requirements(self):
        self.requires("gtest/1.11.0")
        self.requires("cjson/1.7.15")

    def source(self):
        git = Git(self)
        git.clone(url=self.url, target=".")

    def generate(self):
        """生成 conan_toolchain.cmake 和依赖配置文件"""
        tc = CMakeToolchain(self)
        
        # 使用 Conan 内置的 Version 对象解析版本号
        v = Version(self.version)
        
        # 填充变量给 CMake (处理 None 的情况确保安全)
        tc.variables["RUNTIME_VERSION_MAJOR"] = v.major or 0
        tc.variables["RUNTIME_VERSION_MINOR"] = v.minor or 0
        tc.variables["RUNTIME_VERSION_PATCH"] = v.patch or 0
        
        # 处理预发布版本号 (如 1.0.0-alpha 中的 alpha)
        if v.pre:
            tc.variables["RUNTIME_VERSION_PRERELEASE"] = str(v.pre)
            
        tc.variables["BUILD_UNITTEST"] = (self.settings.build_type == "Debug")
        tc.generate()

        # 生成 FindXXX.cmake 或 XXXConfig.cmake
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        # 优先使用 CMakeLists.txt 里的 install() 逻辑
        cmake.install()
        
        # 手动备份拷贝（如果 CMakeLists 没有写 install 逻辑）
        copy(self, "*.so*", src=self.build_folder, dst=os.path.join(self.package_folder, "lib"), keep_path=False)
        copy(self, "*.a", src=self.build_folder, dst=os.path.join(self.package_folder, "lib"), keep_path=False)
        copy(self, "*", src=os.path.join(self.source_folder, "include"), dst=os.path.join(self.package_folder, "include"))

    def package_info(self):
        self.cpp_info.libs = ["runtime-service"]