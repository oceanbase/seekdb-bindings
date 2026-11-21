{
  "targets": [
    {
      "target_name": "seekdb_native",
      "sources": ["native/seekdb_node.c"],
      "include_dirs": ["../lib/include"],
      "cflags": ["-std=c11", "-Wall", "-Wextra"],
      "conditions": [
        ["OS=='mac'", {
          "libraries": ["-L<(module_root_dir)/../build/node-sdk", "-lseekdb"],
          "xcode_settings": {
            "OTHER_CFLAGS": ["-std=c11", "-Wall", "-Wextra"],
            "OTHER_LDFLAGS": ["-Wl,-rpath,@loader_path"]
          }
        }],
        ["OS=='linux'", {
          "libraries": ["-L<(module_root_dir)/../build/node-sdk", "-lseekdb"],
          "ldflags": ["-Wl,--as-needed", "-Wl,-rpath,'$$ORIGIN'"]
        }],
        ["OS=='win'", {
          "libraries": ["<(module_root_dir)/../build/node-sdk/Release/seekdb.lib"]
        }]
      ]
    }
  ]
}
