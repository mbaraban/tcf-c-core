@rem ***************************************************************************
@rem Copyright (c) 2011 Wind River Systems, Inc. and others.
@rem All rights reserved. This program and the accompanying materials
@rem are made available under the terms of the Eclipse Public License v1.0
@rem and Eclipse Distribution License v1.0 which accompany this distribution.
@rem The Eclipse Public License is available at
@rem http://www.eclipse.org/legal/epl-v10.html
@rem and the Eclipse Distribution License is available at
@rem http://www.eclipse.org/org/documents/edl-v10.php.
@rem You may elect to redistribute this code under either of these licenses.
@rem
@rem Contributors:
@rem     Wind River Systems - initial API and implementation
@rem ***************************************************************************
@echo off
REM The Visual C++ compiler must be in your path
REM This is handled automatically with CDT's Visual C++ integration

if exist msvc-debug goto checkmk
mkdir msvc-debug

:checkmk
cd msvc-debug
if exist Makefile goto dobuild
@echo on
cmake -DCMAKE_BUILD_TYPE=Debug ..\..

:dobuild
@echo on
nmake %*
