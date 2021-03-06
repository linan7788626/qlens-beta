#topdir = ./MUMPS_5.0.1
#libdir = $(topdir)/lib
#include $(topdir)/Makefile.inc
#LIBMUMPS_COMMON = $(libdir)/libmumps_common$(PLAT)$(LIBEXT)
#LIBDMUMPS = $(libdir)/libdmumps$(PLAT)$(LIBEXT) $(LIBMUMPS_COMMON)

#include ./qlens_umfpack_config.mk

# Version without MUMPS
default: qlens mkdist cosmocalc 
CCOMP = g++
#CCOMP = mpicxx -DUSE_MPI
#OPTS = -w -fopenmp -O3
#OPTS = -g -w -fopenmp #for debugging
OPTS = -w -O3
OPTS_NO_OPT = -w
#OPTS = -w -g
#FLAGS = -DUSE_FITS -DUSE_OPENMP -DUSE_UMFPACK
#FLAGS = -DUSE_FITS -DUSE_OPENMP
#OTHERLIBS =  -lm -lreadline -ltcmalloc -lcfitsio
OTHERLIBS =  -lm -lreadline
LINKLIBS = $(OTHERLIBS)

# Version with MUMPS
#default: qlens mkdist cosmocalc
#CCOMP = mpicxx -DUSE_MPI
##OPTS = -w -O3 -fopenmp
#OPTS = -w -O3 -fopenmp
#FLAGS = -DUSE_OPENMP -DUSE_MUMPS -DUSE_FITS -DUSE_UMFPACK
#CMUMPS = $(INCS) $(CDEFS) -I. -I$(topdir)/include -I$(topdir)/src
#MUMPSLIBS = $(LIBDMUMPS) $(LORDERINGS) $(LIBS) $(LIBBLAS) $(LIBOTHERS) -lgfortran
#OTHERLIBS =  -lm -lreadline -lcfitsio -ltcmalloc
##OTHERLIBS =  -lm -lreadline -lcfitsio
#LINKLIBS = $(MUMPSLIBS) $(OTHERLIBS)

CC   := $(CCOMP) $(OPTS) $(UMFOPTS) $(FLAGS) $(CMUMPS) $(INC) 
CC_NO_OPT   := $(CCOMP) $(OPTS_NO_OPT) $(UMFOPTS) $(FLAGS) $(CMUMPS) $(INC) 
CL   := $(CCOMP) $(OPTS) $(UMFOPTS) $(FLAGS)
GCC   := g++ -w -O3

objects = qlens.o commands.o lens.o imgsrch.o pixelgrid.o cg.o mcmchdr.o \
				profile.o models.o sbprofile.o errors.o brent.o sort.o rand.o gauss.o \
				romberg.o spline.o trirectangle.o GregsMathHdr.o hyp_2F1.o cosmo.o \
				simplex.o powell.o

mkdist_objects = mkdist.o mcmceval.o
mkdist_shared_objects = GregsMathHdr.o errors.o hyp_2F1.o
cosmocalc_objects = cosmocalc.o
cosmocalc_shared_objects = errors.o spline.o romberg.o cosmo.o

qlens: $(objects) $(LIBDMUMPS)
	$(CL) -o qlens $(OPTL) $(objects) $(LINKLIBS) $(UMFPACK) $(UMFLIBS) 

mkdist: $(mkdist_objects)
	$(GCC) -o mkdist $(mkdist_objects) $(mkdist_shared_objects) -lm

cosmocalc: $(cosmocalc_objects)
	$(GCC) -o cosmocalc $(cosmocalc_objects) $(cosmocalc_shared_objects) -lm

mumps:
	(cd MUMPS_5.0.1; $(MAKE))

qlens.o: qlens.cpp qlens.h
	$(CC) -c qlens.cpp

commands.o: commands.cpp qlens.h lensvec.h profile.h
	$(CC_NO_OPT) -c commands.cpp

lens.o: lens.cpp profile.h qlens.h pixelgrid.h lensvec.h matrix.h simplex.h powell.h mcmchdr.h cosmo.h
	$(CC) -c lens.cpp

imgsrch.o: imgsrch.cpp qlens.h lensvec.h
	$(CC) -c imgsrch.cpp

pixelgrid.o: pixelgrid.cpp lensvec.h pixelgrid.h qlens.h matrix.h cg.h
	$(CC) -c pixelgrid.cpp

cg.o: cg.cpp cg.h
	$(CC) -c cg.cpp

mcmchdr.o: mcmchdr.cpp mcmchdr.h GregsMathHdr.h random.h
	$(CC) -c mcmchdr.cpp

profile.o: profile.h profile.cpp lensvec.h
	$(GCC) -c profile.cpp

models.o: profile.h models.cpp
	$(GCC) -c models.cpp

sbprofile.o: sbprofile.h sbprofile.cpp
	$(GCC) -c sbprofile.cpp

errors.o: errors.cpp errors.h
	$(GCC) -c errors.cpp

brent.o: brent.h brent.cpp
	$(GCC) -c brent.cpp

simplex.o: simplex.h rand.h simplex.cpp
	$(GCC) -c simplex.cpp

powell.o: powell.h rand.h powell.cpp
	$(GCC) -c powell.cpp

sort.o: sort.h sort.cpp
	$(GCC) -c sort.cpp

rand.o: rand.h rand.cpp
	$(GCC) -c rand.cpp

gauss.o: gauss.cpp gauss.h
	$(GCC) -c gauss.cpp

romberg.o: romberg.cpp romberg.h
	$(GCC) -c romberg.cpp

spline.o: spline.cpp spline.h errors.h
	$(GCC) -c spline.cpp

trirectangle.o: trirectangle.cpp lensvec.h trirectangle.h
	$(GCC) -c trirectangle.cpp

GregsMathHdr.o: GregsMathHdr.cpp GregsMathHdr.h
	$(GCC) -c GregsMathHdr.cpp

mcmceval.o: mcmceval.cpp mcmceval.h GregsMathHdr.h random.h errors.h
	$(GCC) -c mcmceval.cpp

mkdist.o: mkdist.cpp mcmceval.h errors.h
	$(GCC) -c mkdist.cpp

hyp_2F1.o: hyp_2F1.cpp hyp_2F1.h complex_functions.h
	$(GCC) -c hyp_2F1.cpp

cosmocalc.o: cosmocalc.cpp errors.h cosmo.h
	$(GCC) -c cosmocalc.cpp

cosmo.o: cosmo.cpp cosmo.h
	$(GCC) -c cosmo.cpp

clean_qlens:
	rm qlens $(objects)

clean:
	rm qlens mkdist cosmocalc $(objects) $(mkdist_objects) $(cosmocalc_objects)

clmain:
	rm qlens.o

vim_run:
	qlens

