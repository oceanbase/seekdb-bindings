{
  "variables": {
    "node_addon_api_include%": "<!(node -e \"process.stdout.write(require('node-addon-api').include.replace(/[\\\"']/g, ''))\")",
    "seekdb_lib_dir%": "<!(node -e \"process.stdout.write(require('path').resolve(process.env.SEEKDB_LIB_DIR || '../build'))\")"
  },
  "targets": [
    {
      "target_name": "seekdb",
      "sources": [
        "src/addon.cpp",
        "src/instance.cpp",
        "src/connection.cpp",
        "src/cursor.cpp",
        "src/types.cpp"
      ],
      "include_dirs": [
        "<(node_addon_api_include)",
        "../lib/include"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS"
      ],
      "cflags_cc": [
        "-std=c++17",
        "-fexceptions",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter"
      ],
      "conditions": [
        [
          "OS=='mac'",
          {
            "xcode_settings": {
              "MACOSX_DEPLOYMENT_TARGET": "15.6",
              "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
              "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
              "OTHER_CFLAGS": [
                "-fexceptions"
              ],
              "OTHER_LDFLAGS": [
                "-Wl,-rpath,@loader_path"
              ]
            }
          }
        ],
        [
          "OS=='linux'",
          {
            "ldflags": [
              "-Wl,-rpath,'$$ORIGIN'"
            ],
            "cflags": [
              "-fexceptions"
            ]
          }
        ]
      ],
      "libraries": [
        "-L<(seekdb_lib_dir)",
        "-lseekdb"
      ]
    }
  ]
}