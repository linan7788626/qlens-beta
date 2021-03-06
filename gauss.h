#ifndef GAUSS_H
#define GAUSS_H

class GaussianIntegral
{
     protected:
         double *weights;
         double *points;
         int numberOfPoints;
			static const double EPS;
			static const double RT4M_PI_INV;
			double Gamma(const double xx);
          
     public:
			 GaussianIntegral();
          GaussianIntegral(int);
          double output(double (GaussianIntegral::*)(double));
          ~GaussianIntegral();
          double NIntegrate(double (GaussianIntegral::*)(double), double, double);
          double NIntegrateInf(double (GaussianIntegral::*)(double));
};

class GaussLegendre : public GaussianIntegral
{
     public:
		  	 GaussLegendre() : GaussianIntegral() {}
          GaussLegendre(int);
          void SetGaussLegendre(int);
			 void SetGaussLegendre(int N, double *points_in, double *weights_in);
};

class GaussHermite : public GaussianIntegral
{
     public:
		  	 GaussHermite() : GaussianIntegral() {}
          GaussHermite(int);
          void SetGaussHermite(int);
};

class GaussLaguerre : public GaussianIntegral {public: GaussLaguerre(const double, int);};
class GaussJacobi : public GaussianIntegral {public: GaussJacobi(const double, const double, int);};
class GaussChebyshev : public GaussianIntegral {public: GaussChebyshev(int);};

#endif // GAUSS_H
