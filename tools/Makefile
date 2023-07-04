TOP=..

include $(TOP)/configure/CONFIG
# cfg/ sometimes isn't correctly included due to a Base bug
# so we do here (maybe again) as workaround
-include $(wildcard $(TOP)/cfg/CONFIG*))
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

# access to private headers
USR_CPPFLAGS += -I$(TOP)/src

PROD_LIBS += pvxs Com

PROD += pvxvct
pvxvct_SRCS += pvxvct.cpp

PROD += pvxinfo
pvxinfo_SRCS += info.cpp

PROD += pvxget
pvxget_SRCS += get.cpp

PROD += pvxmonitor
pvxmonitor_SRCS += monitor.cpp

PROD += pvxput
pvxput_SRCS += put.cpp

PROD += pvxcall
pvxcall_SRCS += call.cpp

PROD += pvxlist
pvxlist_SRCS += list.cpp

PROD += pvxmshim
pvxmshim_SRCS += mshim.cpp

#===========================

include $(TOP)/configure/RULES
-include $(wildcard $(TOP)/cfg/RULES*))
#----------------------------------------
#  ADD RULES AFTER THIS LINE

