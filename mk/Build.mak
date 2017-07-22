ifeq ($(SRC), )
	SRC = $(wildcard *.cpp)
endif

OBJDIR := $(OBJDIR)$(PROJECT_NAME)/

OBJS = $(addprefix $(OBJDIR), $(SRC:.cpp=.o))
DEPS = $(addprefix $(OBJDIR), $(SRC:.cpp=.d))


CXX = g++
LD = g++
AR = ar
CFLAGS := $(CFLAGS) \
	-Wall \
	-Wno-trigraphs \
	-Wno-unused-result \
	-fconcepts \
	-std=c++1z \
	-pthread -fPIC \
	-DTS_PROJECT_NAME=$(PROJECT_NAME)

#	-std=c++1y \
#	-fext-numeric-literals \

#	-std=gnu++11 \

LDFLAGS := $(LDFLAGS) -lpthread -static-libstdc++ -latomic


#-lboost_regex

ifeq ($(CONFIG), Release)
	CFLAGS := $(CFLAGS) $(CFLAGS_RELEASE) -O3 -ffunction-sections -fdata-sections
	LDFLAGS := $(LDFLAGS) $(LDFLAGS_RELEASE) -s -Wl,-gc-sections
else
	CFLAGS := $(CFLAGS) $(CFLAGS_DEBUG) -g
	LDFLAGS := -ggdb -rdynamic $(LDFLAGS) $(LDFLAGS_DEBUG)
endif

INCLUDE := \
	$(addprefix -I, $(INCLUDE))

DEPENDS_MAKE := $(DEPENDS:.mak=.mak.depend)
DEPENDS_CLEAN := $(DEPENDS:.mak=.mak.clean)

.PHONY: all

all: build

rebuild: clean build

build: $(PROJECT_NAME)
	@rm -f $(OBJDIR)*.d

clean: $(DEPENDS_CLEAN) clean_$(TARGET)
	@rm -f $(OBJDIR)*.o
	@rm -f $(OBJDIR)*.d

install: install_$(TARGET)

install_shared-lib:
	install $(OUTDIR)$(TARGET_NAME).so $(INSTALL_DIR)

install_static-lib:
	@echo No install for static lib

install_executable:
	@echo No install for executable

clean_static-lib:
	@rm -f  $(LIBDIR)$(TARGET_NAME).a

clean_shared-lib:
	@rm -f  $(OUTDIR)$(TARGET_NAME).so

clean_executable:
	@rm -f  $(OUTDIR)$(TARGET_NAME)



$(PROJECT_NAME): $(DEPENDS_MAKE) $(TARGET_NAME).$(TARGET)

$(TARGET_NAME).static-lib: $(OBJS) $(LIBDIR)$(TARGET_NAME).a

$(TARGET_NAME).shared-lib: $(OBJS) $(LIBS) $(OUTDIR)$(TARGET_NAME).so

$(TARGET_NAME).executable: $(OBJS) $(LIBS) $(OUTDIR)$(TARGET_NAME)

ifeq ($(TARGET), static-lib)

$(LIBDIR)%.a: $(OBJS) | $(LIBDIR)
	$(AR) -s -r $@ $^

endif

$(OUTDIR)%.so : $(OBJS) $(LIBS) | $(OUTDIR)
	$(LD) $(EXTLIBDIR) -shared -o $@ $^ $(LDFLAGS)

$(OUTDIR)%: $(OBJS) $(LIBS) | $(OUTDIR)
	$(CXX) $(EXTLIBDIR) -o $@ $^ $(LDFLAGS)

$(OBJDIR)%.d: %.cpp | $(OBJDIR)
	@$(CXX) -MM $(CFLAGS) -MT '$(OBJDIR)$(patsubst %.cpp,%.o, $<)' $(INCLUDE) $< > $@

-include $(DEPS)

$(OBJDIR)%.o: %.cpp | $(OBJDIR)
	@echo $<
	$(CXX) -c $< -o $@ $(CFLAGS) $(INCLUDE)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OUTDIR):
	@mkdir -p $(OUTDIR)

$(LIBDIR):
	@mkdir -p $(LIBDIR)

$(DEPENDS_MAKE):
	@echo make $(CONFIG) $(patsubst %.mak.depend, %.mak, $@)
	@$(MAKE) --no-print-directory -C $(dir $@) -f $(patsubst %.mak.depend, %.mak, $@)

$(DEPENDS_CLEAN):
	@echo clean $(CONFIG) $(patsubst %.mak.clean, %.mak, $@)
	@$(MAKE) clean --no-print-directory -C $(dir $@) -f $(patsubst %.mak.clean, %.mak, $@)
