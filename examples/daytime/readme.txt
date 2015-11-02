Readme for TCF Agent Daytime Example
------------------------------

The Agent Daytime Example shows how a TCF agent can be customized and extended
with a user-defined service.

The example provides code to build the agent with a custom set of services,
including user-defined "Daytime" service. The service is registered via
the main/services-ext.h extension point.

The example is mainly meant for developer's educational use, the Daytime
service is not meant to be of any other value. There is also a TCF Java 
counterpart of this example, providing a Service Proxy: See 
"org.eclipse.tm.tcf.examples.daytime" for details on extending TCF Java 
binding for the Daytime service.

The example includes:
1. Makefile to build the customized agent.
2. Implementation of DayTime service: tcrf/services/daytime.[hc]
3. Agent configuration header file: tcf/config.h

CDT can be used to edit and build the example project.
Supported agent execution environments: Msys, CygWin, Linux.

The examples below assume running both agent and client on the local host,
but a connection to a remote host works just the same (using 1.2.3.4 as 
and example host address, specify "TCP:1.2.3.4:1534" in the connect command).


Building and Running the Example on Linux
----------------------------
git clone http://git.eclipse.org/gitroot/tcf/org.eclipse.tcf.agent.git
cd org.eclipse.tcf.agent/examples/daytime
make NO_SSL=1 NO_UUID=1
make -C ../../agent NO_SSL=1 NO_UUID=1
obj/*/*/Debug/agent -L- &
../../agent/obj/*/*/Debug/client <<<EOF
connect TCP::1534
services
tcf Daytime getTimeOfDay "de"
EOF


Building and Running the Example on Windows (MSYS)
----------------------------
REM git clone and cd like in Linux example (above), then:
make NO_SSL=1 NO_UUID=1 OPSYS=Msys
make -C ../../agent NO_SSL=1 NO_UUID=1 OPSYS=Msys
start obj/Msys/i686/Debug/agent.exe -L-
../../agent/obj/Msys/i686/Debug/client.exe
> connect TCP::1534
> services
> tcf Daytime getTimeOfDay "de"
> exit


Building a Minimal-Footprint Agent
----------------------------
The "daytime" example is already quite small, but can still be stripped down
by removing debug code (Trace service) as well as the auto-discovery service.
The result is a minimal agent that just has the TCF basic infrastructure
(event loop, JSON, service manager) as well as the minimal daytime service.
This minimal agent is 120KB on Linux-x86_64 and 100KB on ARM (Raspberry Pi):

make NO_SSL=1 NO_UUID=1 CONF=Release CFLAGS="-DENABLE_Trace=0 -DENABLE_Discovery=0 -DSERVICE_FileSystem=0 -DSERVICE_SysMonitor=0"
ls -l obj/GNU/Linux/x86_64/Release/
total 428
-rwxrwxr-x. 1 mober users 123504 Nov 14 12:01 agent
-rw-rw-r--. 1 mober users 298280 Nov 14 12:01 libtcf.a

