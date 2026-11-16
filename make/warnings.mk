# Warnings for the project's own C++, as errors, so a build with a new warning
# fails instead of scrolling past. Never applied to libDaisy or NAM Core.
WARNINGS = -Wall -Wextra -Werror

# The firmware builds add the float checks, after libDaisy's Makefile has set
# C_INCLUDES. -Wdouble-promotion and -Wfloat-conversion catch silent double
# math: the Seed's FPU does double precision, but more slowly and with half
# the registers, and a promoted call like sin() links libm's double version.
# The host tests leave them out, since they measure error in double on
# purpose.
#
# libDaisy's own headers don't build cleanly under these flags. GCC treats a
# directory given with both -I and -isystem as a system directory and reports
# no warnings from it, so this keeps the errors to our own code.
FIRMWARE_WARNINGS = $(WARNINGS) -Wdouble-promotion -Wfloat-conversion $(patsubst -I%,-isystem %,$(filter -I$(LIBDAISY_DIR)%,$(C_INCLUDES)))
