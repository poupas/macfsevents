import os

from setuptools.extension import Extension
from setuptools.command.build_ext import build_ext
from setuptools import setup

def read(fname):
    with open(os.path.join(os.path.dirname(__file__), fname)) as f:
        return f.read()

ext_modules = [
    Extension(name = '_fsevents',
              sources = ['_fsevents.c', 'compat.c'],
              extra_link_args = ["-framework","CoreFoundation",
                               "-framework","CoreServices"],
             ),
    ]

setup(name = "MacFSEvents",
      version = "0.9-dev",
      description = "Thread-based interface to file system observation primitives.",
      long_description = "\n\n".join((read('README.rst'), read('CHANGES.rst'))),
      license = "BSD",
      data_files = [("", ["LICENSE.txt"])],
      author = "Joao Poupino",
      url = 'https://github.com/poupas/macfsevents',
      cmdclass = dict(build_ext=build_ext),
      ext_modules = ext_modules,
      platforms = ["macOS"],
      classifiers = [
        'Development Status :: 4 - Beta',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: BSD License',
        'Operating System :: MacOS :: MacOS X',
        'Programming Language :: C',
        'Programming Language :: Python :: 2.4',
        'Programming Language :: Python :: 2.5',
        'Programming Language :: Python :: 2.6',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3.2',
        'Programming Language :: Python :: 3.3',
        'Topic :: Software Development :: Libraries :: Python Modules',
        'Topic :: System :: Filesystems',
      ],
      zip_safe=False,
      py_modules=['fsevents'],
     )
