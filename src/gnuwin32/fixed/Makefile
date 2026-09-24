include ../MkRules

R_HOME = ../../..

ifeq ($(OS),Windows_NT)
  # FIXME:  Should find a make where $(abspath ...) works and avoid this...
  LOCAL_SOFT := $(shell ../Rpwd.exe $(LOCAL_SOFT))
else
  LOCAL_SOFT := $(abspath $(LOCAL_SOFT))
endif

all: profiles fixhtml fixetc fixai fixdoc svnonly

profiles: $(R_HOME)/library/base/R/Rprofile

$(R_HOME)/library/base/R/Rprofile:  ../../library/profile/Common.R \
../../library/profile/Rprofile.windows
	@$(ECHO) -------- Building $@ from $^--------
	mkdir -p $(R_HOME)/library/base/R
	@$(CAT)  $^ > $@

fixhtml:  html/rwin.html
	$(CP) -p html/rwin.html $(R_HOME)/doc/html/index.html
	@$(ECHO) done > fixhtml

ETC = $(wildcard ./etc/*)
fixetc: $(ETC) ../MkRules Makefile
	$(CP) -p $(ETC) $(R_HOME)/etc
	$(RM) $(R_HOME)/etc/Makeconf
	$(MKDIR) -p $(R_HOME)/etc/$(R_ARCH)
	$(SED) -e 's/WIN = 64/WIN = $(WIN)/' \
	  -e "s/-O3/-O2/" \
	  -e "s/@CSTD@/$(CSTD)/" \
	  -e "s/@EOPTS@/$(EOPTS)/" \
	  -e "s/@SANOPTS@/$(SANOPTS)/" \
	  -e "s|BINPREF =|BINPREF ?= $(BINPREF)|" \
	  -e "s|USE_LLVM =|USE_LLVM ?= $(USE_LLVM)|" \
	  -e "s|COMPILED_BY =|COMPILED_BY = $(COMPILED_BY)|" \
	  -e "s|IMPDIR = bin|IMPDIR = $(IMPDIR)|" \
	  -e "s|LOCAL_SOFT ?=|LOCAL_SOFT ?= $(LOCAL_SOFT)|" \
	  -e "s|R_ARCH =|R_ARCH = $(R_ARCH)|" \
	  -e "s|DT_ARCH =|DT_ARCH = $(DT_ARCH)|" \
	  -e "s|RC_ARCH =|RC_ARCH = $(RC_ARCH)|" \
	  -e "s|M_ARCH =|M_ARCH = $(M_ARCH)|" \
	  -e "s|@SYMPAT@|$(SYMPAT)|" \
	  -e "s|@OPENMP@|$(OPENMP)|" \
	  -e "s|@PTHREAD@|$(PTHREAD)|" \
	  -e "s@NM_FILTER =@NM_FILTER = $(NM_FILTER)@" \
	  -e "s|LTO =|LTO = $(LTO)|" \
	  -e "s|LTO_OPT =|LTO_OPT = $(LTO_OPT)|" \
	  -e "s|FLIBS =|FLIBS = $(FLIBS)|" \
	  etc/Makeconf > $(R_HOME)/etc$(R_ARCH)/Makeconf
	$(SED) -e "s+@BINDIR@+$(BINDIR)+" \
	   -e "s+@USE_LLVM@+$(USE_LLVM)+" \
	   Makeconf > $(R_HOME)/Makeconf
	@$(ECHO) done > fixetc

## Templates for the AI assistant (see etc/Rai.conf).  The model and
## llama-server.exe are not shipped; these just create the layout and
## the default prompt, and never overwrite a file the user has edited.
fixai: ai/system_prompt.txt ai/context/README.txt ai/SETUP.md ai/Start-R.cmd
	-$(MKDIR) -p $(R_HOME)/ai/context $(R_HOME)/ai/models $(R_HOME)/ai/llama
	-test -f $(R_HOME)/ai/system_prompt.txt || $(CP) -p ai/system_prompt.txt $(R_HOME)/ai
	-test -f $(R_HOME)/ai/context/README.txt || $(CP) -p ai/context/README.txt $(R_HOME)/ai/context
	-$(CP) -p ai/SETUP.md ai/Start-R.cmd $(R_HOME)/ai
	@$(ECHO) done > fixai

fixdoc: $(wildcard ../CHANGES?)
	$(CP) -p $^ $(R_HOME)/doc
	@$(ECHO) done > fixdoc

svnonly:
	@$(MAKE) -C ../../../doc/manual -f Makefile.win svnonly

clean:
	$(RM) *~ */*~ fixhtml fixbin fixetc fixai fixshare fixdoc

distclean:
	$(RM) $(R_HOME)/doc/CHANGES* $(R_HOME)/Makeconf
