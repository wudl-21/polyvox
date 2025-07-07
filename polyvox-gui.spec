# polyvox-gui.spec

# -*- mode: python ; coding: utf-8 -*-

# --- 新增：导入 PyInstaller 的辅助函数 ---
from PyInstaller.utils.hooks import collect_submodules

block_cipher = None

# --- 修复：使用 collect_submodules 来确保 numpy 和 scipy 被完整打包 ---
a = Analysis(['script/main_gui.py'],
             pathex=['D:\\Projects\\teardown\\polyvox'],
             binaries=[],
             datas=[
                ('bin/polyvox.exe', 'bin'),
                ('locale', 'locale'),
                ('img', 'img'),
                ('script/themes', 'script/themes'),
                ('./LICENSE', '.'),
             ],
             hiddenimports=(
                collect_submodules('numpy') +
                collect_submodules('scipy')
             ),
             hookspath=[],
             runtime_hooks=[],
             excludes=[],
             win_no_prefer_redirects=False,
             win_private_assemblies=False,
             cipher=block_cipher,
             noarchive=False)

pyz = PYZ(a.pure, a.zipped_data,
             cipher=block_cipher)
exe = EXE(pyz,
          a.scripts,
          [],
          a.binaries,
          a.datas,
          name='polyvox-gui',
          debug=False,
          bootloader_ignore_signals=False,
          strip=False,
          upx=True,
          upx_exclude=[],
          runtime_tmpdir=None,
          console=False,
          onefile=True,
          icon='img\\icon.ico')