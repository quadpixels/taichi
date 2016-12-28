from taichi.util import get_os_name, get_uuid
from taichi.settings import get_output_directory

import random
import shutil
import os
import sys
import atexit

CREATE_SAND_BOX_ON_WINDOWS = True

if get_os_name() == 'osx':
    if os.path.exists('libtaichi_core.dylib'):
        shutil.copy('libtaichi_core.dylib', 'taichi_core.so')
        sys.path.append(".")
    import taichi_core as tc_core
elif get_os_name() == 'linux':
    if os.path.exists('libtaichi_core.so'):
        shutil.copy('libtaichi_core.so', 'taichi_core.so')
        sys.path.append(".")
    import taichi_core as tc_core
elif get_os_name() == 'win':
    import ctypes

    bin_dir = os.environ['TAICHI_BIN_DIR'] + '/'
    dll_path = bin_dir + '/Release/taichi_core.dll'

    # The problem here is, on windows, when an dll/pyd is loaded, we can not write to it any more...

    # Ridiculous...
    old_wd = os.getcwd()
    os.chdir(bin_dir)

    if os.path.exists(dll_path):
        if CREATE_SAND_BOX_ON_WINDOWS:
            # So let's just create a sandbox for separated core lib development and loading
            dir = get_output_directory() + '/tmp/' + get_uuid() + '/'
            os.mkdir(dir)
            '''
            for fn in os.listdir(bin_dir):
                if fn.endswith('.dll') and fn != 'taichi_core.dll':
                    print dir + fn, bin_dir + fn
                    # Why can we create symbolic links....
                    # if not ctypes.windll.kernel32.CreateSymbolicLinkW(bin_dir + fn, dir + fn, 0):
                    #    raise OSError
                    shutil.copy(bin_dir + fn, dir + fn)
            '''
            shutil.copy(dll_path, dir + 'taichi_core.pyd')
            sys.path.append(dir)
            import taichi_core as tc_core
        else:
            shutil.copy(dll_path, bin_dir + 'taichi_core.pyd')
            sys.path.append(bin_dir)
            import taichi_core as tc_core
    else:
        assert False, "Library taichi_core doesn't exists."

    os.chdir(old_wd)

@atexit.register
def clean_libs():
    pass
