import sys
import subprocess
import os
from subprocess import Popen, PIPE

env_copy = os.environ.copy()

files = [name.strip() for name in sys.stdin.readlines()]

#files = ['src/input.hpp']

enable_shell = False

status = 0
for name in files:
    print("%s: "%name, end='')
    with open(name, 'r') as f:
        original = f.read()
    p1 = subprocess.Popen(["sed", r"s/#pragma omp/\/\/#pragma omp/g", name], shell=enable_shell, stdout=subprocess.PIPE)
    p2 = subprocess.Popen(["clang-format-mp-7.0", "-style=file"],            shell=enable_shell, stdin=p1.stdout, stdout=subprocess.PIPE)
    p3 = subprocess.Popen(["sed", r"s/\/\/ *#pragma omp/#pragma omp/g"],     shell=enable_shell, stdin=p2.stdout, stdout=subprocess.PIPE)
    formatted = p3.communicate()[0].decode('utf-8')
    p3.wait()
    if (original == formatted):
        print("OK\n")
    else:
        print("needs formatting\n")
        status += 1

sys.exit(status)
