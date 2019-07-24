# Copyright (c) 2019-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from __future__ import absolute_import, division, print_function, unicode_literals

import re
import shlex
import sys


def is_windows():
    """ Returns true if the system we are currently running on
    is a Windows system """
    return sys.platform.startswith("win")


def get_linux_type():
    try:
        with open("/etc/os-release") as f:
            data = f.read()
    except EnvironmentError:
        return (None, None)

    os_vars = {}
    for line in data.splitlines():
        parts = line.split("=", 1)
        if len(parts) != 2:
            continue
        key = parts[0].strip()
        value_parts = shlex.split(parts[1].strip())
        if not value_parts:
            value = ""
        else:
            value = value_parts[0]
        os_vars[key] = value

    name = os_vars.get("NAME")
    if name:
        name = name.lower()
        name = re.sub("linux", "", name)
        name = name.strip()

    return "linux", name, os_vars.get("VERSION_ID").lower()


class HostType(object):
    def __init__(self, ostype=None, distro=None, distrovers=None):
        if ostype is None:
            distro = None
            distrovers = None
            if sys.platform.startswith("linux"):
                ostype, distro, distrovers = get_linux_type()
            elif sys.platform.startswith("darwin"):
                ostype = "darwin"
            elif is_windows():
                ostype = "windows"
                distrovers = str(sys.getwindowsversion().major)
            else:
                ostype = sys.platform

        # The operating system type
        self.ostype = ostype
        # The distribution, if applicable
        self.distro = distro
        # The OS/distro version if known
        self.distrovers = distrovers

    def is_windows(self):
        return self.ostype == "windows"

    def is_darwin(self):
        return self.ostype == "darwin"

    def is_linux(self):
        return self.ostype == "linux"

    def as_tuple_string(self):
        return "%s-%s-%s" % (
            self.ostype,
            self.distro or "none",
            self.distrovers or "none",
        )

    @staticmethod
    def from_tuple_string(s):
        ostype, distro, distrovers = s.split("-")
        return HostType(ostype=ostype, distro=distro, distrovers=distrovers)

    def __eq__(self, b):
        return (
            self.ostype == b.ostype
            and self.distro == b.distro
            and self.distrovers == b.distrovers
        )


def context_from_host_tuple(host_tuple=None, facebook_internal=False):
    """ Given an optional host tuple, construct a context appropriate
    for passing to the boolean expression evaluator so that conditional
    sections in manifests can be resolved. """
    if host_tuple is None:
        host_type = HostType()
    elif isinstance(host_tuple, HostType):
        host_type = host_tuple
    else:
        host_type = HostType.from_tuple_string(host_tuple)

    return {
        "os": host_type.ostype,
        "distro": host_type.distro,
        "distro_vers": host_type.distrovers,
        "fb": "on" if facebook_internal else "off",
    }
