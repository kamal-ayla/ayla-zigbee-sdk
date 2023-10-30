# Configure
The default configuration is already present in *.cmake files.

# Build
Run this to build:
```
./kvsd_build_apps.sh ./cmake_c_flags.cmake ./cmake_toolchain_flags.cmake ./cmake_includes.cmake ./cmake_library_path.cmake ./staging_dir.path 2>&1 | tee build.log
```

The output from the build will be present on the screen and in the file build.log.

