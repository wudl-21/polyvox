[//]: # (Version: 1.0.0)

<div align="center">

[**English**](./README.md) | [**中文**](./README.zh.md)

</div>

---

# Polyvox: Model Converter for Teardown

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Release](https://img.shields.io/github/v/release/wudl-21/polyvox.svg)](https://github.com/wudl-21/polyvox/releases/latest)

**Polyvox** is a powerful and user-friendly desktop application designed to streamline the process of converting standard 3D models (`.obj`) into a format compatible with the game Teardown. It not only voxelizes models but also provides granular control over physical materials and rendering properties, automatically generating the necessary `.xml` prefab files for in-game use.

![Polyvox Software Screenshot](img/splash.png)  

---

## ✨ Features

*   **One-Click Conversion**: Simply select an `.obj` file and an output directory to start the conversion.
*   **Robust Workflow**:
    *   All operations are performed in a temporary directory to prevent data loss.
    *   Features atomic file commits, ensuring that your output directory is only modified upon successful completion.
    *   The process can be safely aborted at any stage without corrupting existing files.
*   **Advanced Material Mapping**:
    *   **Auto-Detect Mode**: Intelligently infers Teardown physical materials from material names in the `.mtl` file (e.g., "wood", "metal").
    *   **Manual Mode**: A powerful GUI allows you to manually assign Teardown physical tags (e.g., `$TD_wood`, `$TD_metal`) for each material.
    *   **Batch Editing**: Supports multi-selection for setting properties on many materials at once.
    *   **Advanced Sorting & Filtering**: Easily manage large numbers of materials with sorting modes (by name, type) and a real-time search filter.
*   **Fine-Grained Render Control**:
    *   Independently set the render type (`diffuse`, `metal`, `glass`, `emissive`) for each material in the `.vox` file.
    *   Precisely adjust render properties like `roughness`, `metalness`, `ior` (index of refraction), and `emission`.
*   **Preset System**: Save a default set of render types and properties for each Teardown physical material in the Preferences, ensuring a consistent art style across projects.
*   **Modern User Interface**:
    *   Built with PySide6 (Qt) for a beautiful and responsive interface.
    *   Supports **Light** and **Dark** themes, with an option to automatically follow your system's setting.
    *   **Multi-Language Support** (English/Chinese), with an easily extendable localization system.
*   **Non-Blocking & Responsive**: All time-consuming operations run in a background thread, keeping the UI responsive.
*   **Real-time Feedback**: A progress bar and detailed log window provide real-time updates on the conversion status.

## 🚀 How to Use (For Users)

1.  **Download**: Go to the project's [Releases Page](https://github.com/wudl-21/polyvox/releases/latest) and download the latest `polyvox-gui.exe` executable.
2.  **Run**: Double-click `polyvox-gui.exe` to start the application.
3.  **Select Model**: Click the "Browse" button or **drag and drop** your `.obj` file onto the input field.
    *   **Important**: Ensure the `.obj` file, its referenced `.mtl` file, and all texture maps are located in the **same folder**.
4.  **Set Output**: Choose an output directory for the generated `.vox` and `.xml` files.
5.  **Adjust Voxel Size**: Set the "Voxel Size" as needed. Smaller values result in a more detailed model but increase performance cost.
6.  **(Optional) Configure Materials**:
    *   **Automatic**: Keep "Auto-detect physical materials" checked for automatic tag assignment based on material names.
    *   **Manual**: Uncheck the box and click "Customize Materials...". In the dialog, you can assign physical tags and detailed render properties for each material.
7.  **Start Conversion**: Click the "Start Conversion" button.
8.  **Done**: Wait for the progress bar to complete. The application will notify you and offer to open the output folder. The output contains two directories: `vox` (containing all `.vox` files) and `prefab` (containing the final assembled `.xml` file). Simply copy these two directories into your Teardown mod's root directory. Then, in the Teardown editor, create a new `instance` and select the generated `.xml` file to import your model. Your mod's directory should look like this:

```
YOUR_MOD/
├── prefab/
│   └── my_model.xml
├── vox/
│   └── my_model/
│       ├── surface_1.vox
│       ├── surface_2.vox
│       └── ...
└── main.xml (and other mod files)
```

## 🛠️ Build from Source (For Developers)

If you wish to run or modify the program from the source code, follow these steps:

1.  **Clone the repository**:
    ```sh
    git clone https://github.com/wudl-21/polyvox.git
    cd polyvox
    ```

2.  **Build the C++ Backend**:
    *   This project uses CMake. Ensure you have CMake and a C++17-compatible compiler (e.g., Visual Studio, GCC, Clang) installed.
    *   Open a terminal and run the following commands:
      ```sh
      cmake -S . -B build
      cmake --build build --config Release
      ```
    *   After a successful build, `polyvox.exe` will be located in the `bin/Release` directory.

3.  **Install Python Dependencies**:
    *   Using a virtual environment is highly recommended.
    ```sh
    conda env create -f environment.yml
    conda activate polyvox
    ```

4.  **Run the GUI**:
    ```sh
    python script/main_gui.py
    ```

5.  **Package with PyInstaller**:
    *   Ensure PyInstaller is installed (`pip install pyinstaller`).
    *   Use the provided `.spec` file for packaging:
      ```sh
      pyinstaller polyvox-gui.spec --clean
      ```
    *   The final executable will be in the `dist` directory.

## ⚠️ Important Notes

*   **File Paths**: For best compatibility, ensure all your asset paths (`.obj`, `.mtl`, textures) do not contain complex non-ASCII characters.
*   **Model Position**: The tool automatically centers the model based on its bounding box. You may need to fine-tune the position of the `instance` in the Teardown editor.
*   **Performance**: Very complex models (>10,000 faces) or extremely small voxel sizes can consume significant memory and processing time. It is recommended to use models with a reasonable face count and stick to the default voxel size unless necessary.
*   **Temporary Files**: By default, the application uses the system's temporary directory for processing. You can specify a custom path in `Preferences -> General`.

## 📜 License

This project is licensed under the [MIT License](LICENSE).