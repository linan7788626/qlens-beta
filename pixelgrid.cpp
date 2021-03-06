#include "trirectangle.h"
#include "cg.h"
#include "pixelgrid.h"
#include "profile.h"
#include "qlens.h"
#include "mathexpr.h"
#include "errors.h"
#include <stdio.h>

#ifdef USE_UMFPACK
#include "umfpack.h"
#endif

#ifdef USE_FITS
#include "fitsio.h"
#endif

#include <cmath>
#include <iostream>
#include <fstream>
#include <iomanip>
#define USE_COMM_WORLD -987654
#define MUMPS_SILENT -1
#define MUMPS_OUTPUT 6
#define JOB_INIT -1
#define JOB_END -2
using namespace std;

int SourcePixelGrid::nthreads;
const int SourcePixelGrid::max_levels = 6;
int SourcePixelGrid::number_of_pixels;
int *SourcePixelGrid::imin, *SourcePixelGrid::imax, *SourcePixelGrid::jmin, *SourcePixelGrid::jmax;
TriRectangleOverlap *SourcePixelGrid::trirec;
InterpolationCells *SourcePixelGrid::nearest_interpolation_cells;
lensvector **SourcePixelGrid::interpolation_pts[3];
int *SourcePixelGrid::n_interpolation_pts;
double SourcePixelGrid::zfactor;
double ImagePixelGrid::zfactor;

// parameters for creating the recursive grid
double SourcePixelGrid::xcenter, SourcePixelGrid::ycenter;
double SourcePixelGrid::srcgrid_xmin, SourcePixelGrid::srcgrid_xmax, SourcePixelGrid::srcgrid_ymin, SourcePixelGrid::srcgrid_ymax;
int SourcePixelGrid::u_split_initial, SourcePixelGrid::w_split_initial;
double SourcePixelGrid::min_cell_area;

// NOTE!!! It would be better to make a few of these (e.g. levels) non-static and contained in the zeroth-level grid, and just give all the subcells a pointer to the zeroth-level grid.
// That way, you can create multiple source grids and they won't interfere with each other.
int SourcePixelGrid::levels, SourcePixelGrid::splitlevels;
lensvector SourcePixelGrid::d1, SourcePixelGrid::d2, SourcePixelGrid::d3, SourcePixelGrid::d4;
double SourcePixelGrid::product1, SourcePixelGrid::product2, SourcePixelGrid::product3;
ImagePixelGrid* SourcePixelGrid::image_pixel_grid;
bool SourcePixelGrid::regrid;
int *SourcePixelGrid::maxlevs;
lensvector ***SourcePixelGrid::xvals_threads;
lensvector ***SourcePixelGrid::corners_threads;

ifstream SourcePixelGrid::sb_infile;

/***************************************** Functions in class SourcePixelGrid ****************************************/

void SourcePixelGrid::set_splitting(int usplit0, int wsplit0, double min_cs)
{
	u_split_initial = usplit0;
	w_split_initial = wsplit0;
	if ((u_split_initial < 2) or (w_split_initial < 2)) die("source grid dimensions cannot be smaller than 2 along either direction");
	min_cell_area = min_cs;
}

void SourcePixelGrid::allocate_multithreaded_variables(const int& threads)
{
	nthreads = threads;
	trirec = new TriRectangleOverlap[nthreads];
	imin = new int[nthreads];
	imax = new int[nthreads];
	jmin = new int[nthreads];
	jmax = new int[nthreads];
	nearest_interpolation_cells = new InterpolationCells[nthreads];
	int i,j;
	for (i=0; i < 3; i++) interpolation_pts[i] = new lensvector*[nthreads];
	n_interpolation_pts = new int[threads];
	maxlevs = new int[threads];
	xvals_threads = new lensvector**[threads];
	for (j=0; j < threads; j++) {
		xvals_threads[j] = new lensvector*[3];
		for (i=0; i <= 2; i++) xvals_threads[j][i] = new lensvector[3];
	}
	corners_threads = new lensvector**[nthreads];
	for (int i=0; i < nthreads; i++) corners_threads[i] = new lensvector*[4];
}

void SourcePixelGrid::deallocate_multithreaded_variables()
{
	delete[] trirec;
	delete[] imin;
	delete[] imax;
	delete[] jmin;
	delete[] jmax;
	delete[] nearest_interpolation_cells;
	delete[] maxlevs;
	for (int i=0; i < 3; i++) delete[] interpolation_pts[i];
	delete[] n_interpolation_pts;
	int i,j;
	for (j=0; j < nthreads; j++) {
		for (i=0; i <= 2; i++) delete[] xvals_threads[j][i];
		delete[] xvals_threads[j];
		delete[] corners_threads[j];
	}
	delete[] xvals_threads;
	delete[] corners_threads;
}

SourcePixelGrid::SourcePixelGrid(Lens* lens_in, double x_min, double x_max, double y_min, double y_max) : lens(lens_in)	// use for top-level cell only; subcells use constructor below
{
// this constructor is used for a Cartesian grid
	center_pt = 0;
	// For the Cartesian grid, u = x, w = y
	u_N = u_split_initial;
	w_N = w_split_initial;
	level = 0;
	levels = 0;
	ii=jj=0;
	cell = NULL;
	parent_cell = NULL;
	maps_to_image_pixel = false;
	maps_to_image_window = false;
	active_pixel = false;
	zfactor = lens->reference_zfactor;

	for (int i=0; i < 4; i++) {
		corner_pt[i]=0;
		neighbor[i]=NULL;
	}

	xcenter = 0.5*(x_min+x_max);
	ycenter = 0.5*(y_min+y_max);
	srcgrid_xmin = x_min; srcgrid_xmax = x_max;
	srcgrid_ymin = y_min; srcgrid_ymax = y_max;

	double x, y, xstep, ystep;
	xstep = (x_max-x_min)/u_N;
	ystep = (y_max-y_min)/w_N;

	lensvector **firstlevel_xvals = new lensvector*[u_N+1];
	int i,j;
	for (i=0, x=x_min; i <= u_N; i++, x += xstep) {
		firstlevel_xvals[i] = new lensvector[w_N+1];
		for (j=0, y=y_min; j <= w_N; j++, y += ystep) {
			firstlevel_xvals[i][j][0] = x;
			firstlevel_xvals[i][j][1] = y;
		}
	}

	cell = new SourcePixelGrid**[u_N];
	for (i=0; i < u_N; i++)
	{
		cell[i] = new SourcePixelGrid*[w_N];
		for (j=0; j < w_N; j++)
		{
			cell[i][j] = new SourcePixelGrid(lens,firstlevel_xvals,i,j,1,this);
		}
	}
	levels++;
	assign_firstlevel_neighbors();
	number_of_pixels = u_N*w_N;
	for (int i=0; i < u_N+1; i++)
		delete[] firstlevel_xvals[i];
	delete[] firstlevel_xvals;
}

SourcePixelGrid::SourcePixelGrid(Lens* lens_in, string pixel_data_fileroot, const double& minarea_in) : lens(lens_in)	// use for top-level cell only; subcells use constructor below
{
	min_cell_area = minarea_in;
	string info_filename = pixel_data_fileroot + ".info";
	ifstream infofile(info_filename.c_str());
	double cells_per_pixel;
	infofile >> u_split_initial >> w_split_initial >> cells_per_pixel;
	infofile >> srcgrid_xmin >> srcgrid_xmax >> srcgrid_ymin >> srcgrid_ymax;

	// this constructor is used for a Cartesian grid
	center_pt = 0;
	// For the Cartesian grid, u = x, w = y
	u_N = u_split_initial;
	w_N = w_split_initial;
	level = 0;
	levels = 0;
	ii=jj=0;
	cell = NULL;
	parent_cell = NULL;
	maps_to_image_pixel = false;
	maps_to_image_window = false;
	active_pixel = false;
	zfactor = lens->reference_zfactor;

	for (int i=0; i < 4; i++) {
		corner_pt[i]=0;
		neighbor[i]=NULL;
	}

	xcenter = 0.5*(srcgrid_xmin+srcgrid_xmax);
	ycenter = 0.5*(srcgrid_ymin+srcgrid_ymax);

	double x, y, xstep, ystep;
	xstep = (srcgrid_xmax-srcgrid_xmin)/u_N;
	ystep = (srcgrid_ymax-srcgrid_ymin)/w_N;

	lensvector **firstlevel_xvals = new lensvector*[u_N+1];
	int i,j;
	for (i=0, x=srcgrid_xmin; i <= u_N; i++, x += xstep) {
		firstlevel_xvals[i] = new lensvector[w_N+1];
		for (j=0, y=srcgrid_ymin; j <= w_N; j++, y += ystep) {
			firstlevel_xvals[i][j][0] = x;
			firstlevel_xvals[i][j][1] = y;
		}
	}

	cell = new SourcePixelGrid**[u_N];
	for (i=0; i < u_N; i++)
	{
		cell[i] = new SourcePixelGrid*[w_N];
		for (j=0; j < w_N; j++)
		{
			cell[i][j] = new SourcePixelGrid(lens,firstlevel_xvals,i,j,1,this);
		}
	}
	levels++;
	assign_firstlevel_neighbors();
	number_of_pixels = u_N*w_N;

	string sbfilename = pixel_data_fileroot + ".sb";
	sb_infile.open(sbfilename.c_str());
	read_surface_brightness_data();
	sb_infile.close();
	for (int i=0; i < u_N+1; i++)
		delete[] firstlevel_xvals[i];
	delete[] firstlevel_xvals;
}

void SourcePixelGrid::read_surface_brightness_data()
{
	double sb;
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			sb_infile >> sb;
			if (sb==-1e30) // I can't think of a better dividing value to use right now, so -1e30 is what I am using at the moment
			{
				cell[i][j]->split_cells(2,2,0);
				cell[i][j]->read_surface_brightness_data();
			} else {
				cell[i][j]->surface_brightness = sb;
			}
		}
	}
}

// ***NOTE: the following constructor should NOT be used because there are static variables (e.g. levels), so more than one source grid
// is a bad idea. To make this work, you need to make those variables non-static and contained in the zeroth-level grid (and give subcells
// a pointer to the zeroth-level grid).
SourcePixelGrid::SourcePixelGrid(Lens* lens_in, SourcePixelGrid* input_pixel_grid) : lens(lens_in)	// use for top-level cell only; subcells use constructor below
{
	// these are all static anyway, so this might be superfluous
	min_cell_area = input_pixel_grid->min_cell_area;
	u_split_initial = input_pixel_grid->u_split_initial;
	w_split_initial = input_pixel_grid->w_split_initial;
	srcgrid_xmin = input_pixel_grid->srcgrid_xmin;
	srcgrid_xmax = input_pixel_grid->srcgrid_xmax;
	srcgrid_ymin = input_pixel_grid->srcgrid_ymin;
	srcgrid_ymax = input_pixel_grid->srcgrid_ymax;

	// this constructor is used for a Cartesian grid
	center_pt = 0;
	// For the Cartesian grid, u = x, w = y
	u_N = u_split_initial;
	w_N = w_split_initial;
	level = 0;
	levels = 0;
	ii=jj=0;
	cell = NULL;
	parent_cell = NULL;
	maps_to_image_pixel = false;
	maps_to_image_window = false;
	active_pixel = false;

	for (int i=0; i < 4; i++) {
		corner_pt[i]=0;
		neighbor[i]=NULL;
	}

	xcenter = 0.5*(srcgrid_xmin+srcgrid_xmax);
	ycenter = 0.5*(srcgrid_ymin+srcgrid_ymax);

	double x, y, xstep, ystep;
	xstep = (srcgrid_xmax-srcgrid_xmin)/u_N;
	ystep = (srcgrid_ymax-srcgrid_ymin)/w_N;

	lensvector **firstlevel_xvals = new lensvector*[u_N+1];
	int i,j;
	for (i=0, x=srcgrid_xmin; i <= u_N; i++, x += xstep) {
		firstlevel_xvals[i] = new lensvector[w_N+1];
		for (j=0, y=srcgrid_ymin; j <= w_N; j++, y += ystep) {
			firstlevel_xvals[i][j][0] = x;
			firstlevel_xvals[i][j][1] = y;
		}
	}

	cell = new SourcePixelGrid**[u_N];
	for (i=0; i < u_N; i++)
	{
		cell[i] = new SourcePixelGrid*[w_N];
		for (j=0; j < w_N; j++)
		{
			cell[i][j] = new SourcePixelGrid(lens,firstlevel_xvals,i,j,1,this);
		}
	}
	levels++;
	assign_firstlevel_neighbors();
	number_of_pixels = u_N*w_N;
	copy_source_pixel_grid(input_pixel_grid); // this copies the surface brightnesses and splits the source pixels in the same manner as the input grid
	assign_all_neighbors();

	for (int i=0; i < u_N+1; i++)
		delete[] firstlevel_xvals[i];
	delete[] firstlevel_xvals;
}

void SourcePixelGrid::copy_source_pixel_grid(SourcePixelGrid* input_pixel_grid)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (input_pixel_grid->cell[i][j]->cell != NULL) {
				cell[i][j]->split_cells(input_pixel_grid->cell[i][j]->u_N,input_pixel_grid->cell[i][j]->w_N,0);
				cell[i][j]->copy_source_pixel_grid(input_pixel_grid->cell[i][j]);
			} else {
				cell[i][j]->surface_brightness = input_pixel_grid->cell[i][j]->surface_brightness;
			}
		}
	}
}

SourcePixelGrid::SourcePixelGrid(Lens* lens_in, lensvector** xij, const int& i, const int& j, const int& level_in, SourcePixelGrid* parent_ptr)
{
	u_N = 1;
	w_N = 1;
	level = level_in;
	cell = NULL;
	ii=i; jj=j; // store the index carried by this cell in the grid of the parent cell
	parent_cell = parent_ptr;
	maps_to_image_pixel = false;
	maps_to_image_window = false;
	active_pixel = false;
	lens = lens_in;

	corner_pt[0] = xij[i][j];
	corner_pt[1] = xij[i][j+1];
	corner_pt[2] = xij[i+1][j];
	corner_pt[3] = xij[i+1][j+1];

	center_pt[0] = (corner_pt[0][0] + corner_pt[1][0] + corner_pt[2][0] + corner_pt[3][0]) / 4.0;
	center_pt[1] = (corner_pt[0][1] + corner_pt[1][1] + corner_pt[2][1] + corner_pt[3][1]) / 4.0;
	find_cell_area();
}

void SourcePixelGrid::assign_surface_brightness()
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->assign_surface_brightness();
			else {
				cell[i][j]->surface_brightness = 0;
				for (int k=0; k < lens->n_sb; k++)
					cell[i][j]->surface_brightness += lens->sb_list[k]->surface_brightness(cell[i][j]->center_pt[0],cell[i][j]->center_pt[1]);
			}
		}
	}
}

void SourcePixelGrid::update_surface_brightness(int& index)
{
	for (int j=0; j < w_N; j++) {
		for (int i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->update_surface_brightness(index);
			else {
				if (cell[i][j]->active_pixel) {
					cell[i][j]->surface_brightness = lens->source_surface_brightness[index++];
				} else {
					cell[i][j]->surface_brightness = 0;
				}
			}
		}
	}
}

void SourcePixelGrid::fill_surface_brightness_vector()
{
	int column_j = 0;
	fill_surface_brightness_vector_recursive(column_j);
}

void SourcePixelGrid::fill_surface_brightness_vector_recursive(int& column_j)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->fill_surface_brightness_vector_recursive(column_j);
			else {
				if (cell[i][j]->active_pixel) {
					lens->source_surface_brightness[column_j++] = cell[i][j]->surface_brightness;
				}
			}
		}
	}
}

void SourcePixelGrid::fill_n_image_vector()
{
	int column_j = 0;
	fill_n_image_vector_recursive(column_j);
}

void SourcePixelGrid::fill_n_image_vector_recursive(int& column_j)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->fill_n_image_vector_recursive(column_j);
			else {
				if (cell[i][j]->active_pixel) {
					lens->source_pixel_n_images[column_j++] = cell[i][j]->n_images;
				}
			}
		}
	}
}

ofstream SourcePixelGrid::pixel_surface_brightness_file;
ofstream SourcePixelGrid::pixel_magnification_file;
ofstream SourcePixelGrid::pixel_n_image_file;

void SourcePixelGrid::store_surface_brightness_grid_data(string root)
{
	string img_filename = root + ".sb";
	string info_filename = root + ".info";

	pixel_surface_brightness_file.open(img_filename.c_str());
	write_surface_brightness_to_file();
	pixel_surface_brightness_file.close();

	ofstream pixel_info(info_filename.c_str());
	pixel_info << u_split_initial << " " << w_split_initial << " " << levels << endl;
	pixel_info << srcgrid_xmin << " " << srcgrid_xmax << " " << srcgrid_ymin << " " << srcgrid_ymax << endl;
}

void SourcePixelGrid::write_surface_brightness_to_file()
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) {
				pixel_surface_brightness_file << "-1e30\n";
				cell[i][j]->write_surface_brightness_to_file();
			} else {
				pixel_surface_brightness_file << cell[i][j]->surface_brightness << endl;
			}
		}
	}
}

void SourcePixelGrid::plot_surface_brightness(string root)
{
	string img_filename = root + ".dat";
	string x_filename = root + ".x";
	string y_filename = root + ".y";
	string info_filename = root + ".info";
	string mag_filename = root + ".maglog";
	string n_image_filename = root + ".nimg";

	double x, y, cell_xlength, cell_ylength, xmin, ymin;
	int i, j, k, n_plot_xcells, n_plot_ycells, pixels_per_cell_x, pixels_per_cell_y;
	cell_xlength = cell[0][0]->corner_pt[2][0] - cell[0][0]->corner_pt[0][0];
	cell_ylength = cell[0][0]->corner_pt[1][1] - cell[0][0]->corner_pt[0][1];
	n_plot_xcells = u_N;
	n_plot_ycells = w_N;
	pixels_per_cell_x = 1;
	pixels_per_cell_y = 1;
	for (i=0; i < levels-1; i++) {
		cell_xlength /= 2;
		cell_ylength /= 2;
		n_plot_xcells *= 2;
		n_plot_ycells *= 2;
		pixels_per_cell_x *= 2;
		pixels_per_cell_y *= 2;
	}
	xmin = cell[0][0]->corner_pt[0][0];
	ymin = cell[0][0]->corner_pt[0][1];

	ofstream pixel_xvals(x_filename.c_str());
	for (i=0, x=xmin; i <= n_plot_xcells; i++, x += cell_xlength) pixel_xvals << x << endl;

	ofstream pixel_yvals(y_filename.c_str());
	for (i=0, y=ymin; i <= n_plot_ycells; i++, y += cell_ylength) pixel_yvals << y << endl;

	pixel_surface_brightness_file.open(img_filename.c_str());
	pixel_magnification_file.open(mag_filename.c_str());
	if (lens->n_image_prior) pixel_n_image_file.open(n_image_filename.c_str());
	int line_number;
	for (j=0; j < w_N; j++) {
		for (line_number=0; line_number < pixels_per_cell_y; line_number++) {
			for (i=0; i < u_N; i++) {
				if (cell[i][j]->cell != NULL) {
					cell[i][j]->plot_cell_surface_brightness(line_number,pixels_per_cell_x,pixels_per_cell_y);
				} else {
					for (k=0; k < pixels_per_cell_x; k++) {
						pixel_surface_brightness_file << cell[i][j]->surface_brightness << " ";
						pixel_magnification_file << log(cell[i][j]->total_magnification)/log(10) << " ";
						if (lens->n_image_prior) pixel_n_image_file << cell[i][j]->n_images << " ";
					}
				}
			}
			pixel_surface_brightness_file << endl;
			pixel_magnification_file << endl;
			if (lens->n_image_prior) pixel_n_image_file << endl;
		}
	}
	pixel_surface_brightness_file.close();
	pixel_magnification_file.close();
	pixel_n_image_file.close();

	ofstream pixel_info(info_filename.c_str());
	pixel_info << u_split_initial << " " << w_split_initial << " " << levels << endl;
	pixel_info << srcgrid_xmin << " " << srcgrid_xmax << " " << srcgrid_ymin << " " << srcgrid_ymax << endl;
}

void SourcePixelGrid::plot_cell_surface_brightness(int line_number, int pixels_per_cell_x, int pixels_per_cell_y)
{
	int cell_row, subplot_pixels_per_cell_x, subplot_pixels_per_cell_y, subline_number=line_number;
	subplot_pixels_per_cell_x = pixels_per_cell_x/u_N;
	subplot_pixels_per_cell_y = pixels_per_cell_y/w_N;
	cell_row = line_number / subplot_pixels_per_cell_y;
	subline_number -= cell_row*subplot_pixels_per_cell_y;

	int i,j;
	for (i=0; i < u_N; i++) {
		if (cell[i][cell_row]->cell != NULL) {
			cell[i][cell_row]->plot_cell_surface_brightness(subline_number,subplot_pixels_per_cell_x,subplot_pixels_per_cell_y);
		} else {
			for (j=0; j < subplot_pixels_per_cell_x; j++) {
				pixel_surface_brightness_file << cell[i][cell_row]->surface_brightness << " ";
				pixel_magnification_file << log(cell[i][cell_row]->total_magnification)/log(10) << " ";
				if (lens->n_image_prior) pixel_n_image_file << cell[i][cell_row]->n_images << " ";
			}
		}
	}
}

inline void SourcePixelGrid::find_cell_area()
{
	d1[0] = corner_pt[2][0] - corner_pt[0][0]; d1[1] = corner_pt[2][1] - corner_pt[0][1];
	d2[0] = corner_pt[1][0] - corner_pt[0][0]; d2[1] = corner_pt[1][1] - corner_pt[0][1];
	d3[0] = corner_pt[2][0] - corner_pt[3][0]; d3[1] = corner_pt[2][1] - corner_pt[3][1];
	d4[0] = corner_pt[1][0] - corner_pt[3][0]; d4[1] = corner_pt[1][1] - corner_pt[3][1];
	// split cell into two triangles; cross product of the vectors forming the legs gives area of each triangle, so their sum gives area of cell
	cell_area = 0.5 * (abs(d1 ^ d2) + abs(d3 ^ d4));
}

void SourcePixelGrid::assign_firstlevel_neighbors()
{
	// neighbor index: 0 = i+1 neighbor, 1 = i-1 neighbor, 2 = j+1 neighbor, 3 = j-1 neighbor
	if (level != 0) die("assign_firstlevel_neighbors function must be run from grid level 0");
	int i,j;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) {
			if (j < w_N-1)
				cell[i][j]->neighbor[2] = cell[i][j+1];
			else
				cell[i][j]->neighbor[2] = NULL;

			if (j > 0) 
				cell[i][j]->neighbor[3] = cell[i][j-1];
			else
				cell[i][j]->neighbor[3] = NULL;

			if (i > 0) {
				cell[i][j]->neighbor[1] = cell[i-1][j];
				if (i < u_N-1)
					cell[i][j]->neighbor[0] = cell[i+1][j];
				else
					cell[i][j]->neighbor[0] = NULL;
			} else {
				cell[i][j]->neighbor[1] = NULL;
				cell[i][j]->neighbor[0] = cell[i+1][j];
			}
		}
	}
}

void SourcePixelGrid::assign_neighborhood()
{
	// assign neighbors of this cell, then update neighbors of neighbors of this cell
	// neighbor index: 0 = i+1 neighbor, 1 = i-1 neighbor, 2 = j+1 neighbor, 3 = j-1 neighbor
	assign_level_neighbors(level);
	int l,k;
	for (l=0; l < 4; l++)
		if ((neighbor[l] != NULL) and (neighbor[l]->cell != NULL)) {
		for (k=level; k <= levels; k++) {
			neighbor[l]->assign_level_neighbors(k);
		}
	}
}

void SourcePixelGrid::assign_all_neighbors()
{
	if (level!=0) die("assign_all_neighbors should only be run from level 0");

	int k,i,j;
	for (k=1; k < levels; k++) {
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				cell[i][j]->assign_level_neighbors(k); // we've just created our grid, so we only need to go to level+1
			}
		}
	}
}

void SourcePixelGrid::test_neighbors() // for testing purposes, to make sure neighbors are assigned correctly
{
	int k,i,j;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->test_neighbors();
			else {
				for (k=0; k < 4; k++) {
					if (cell[i][j]->neighbor[k] == NULL)
						cout << "Level " << cell[i][j]->level << " cell (" << i << "," << j << ") neighbor " << k << ": none\n";
					else
						cout << "Level " << cell[i][j]->level << " cell (" << i << "," << j << ") neighbor " << k << ": level " << cell[i][j]->neighbor[k]->level << " (" << cell[i][j]->neighbor[k]->ii << "," << cell[i][j]->neighbor[k]->jj << ")\n";
				}
			}
		}
	}
}

void SourcePixelGrid::assign_level_neighbors(int neighbor_level)
{
	if (cell == NULL) return;
	int i,j;
	if (level < neighbor_level) {
		for (i=0; i < u_N; i++)
			for (j=0; j < w_N; j++)
				cell[i][j]->assign_level_neighbors(neighbor_level);
	} else {
		if (cell==NULL) die("cannot find neighbors if no grid has been set up");
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				if (cell[i][j]==NULL) die("a subcell has been erased");
				if (i < u_N-1)
					cell[i][j]->neighbor[0] = cell[i+1][j];
				else {
					if (neighbor[0] == NULL) cell[i][j]->neighbor[0] = NULL;
					else if (neighbor[0]->cell != NULL) {
						if (j >= neighbor[0]->w_N) cell[i][j]->neighbor[0] = neighbor[0]->cell[0][neighbor[0]->w_N-1]; // allows for possibility that neighbor cell may not be split by the same number of cells
						else cell[i][j]->neighbor[0] = neighbor[0]->cell[0][j];
					} else
						cell[i][j]->neighbor[0] = neighbor[0];
				}

				if (i > 0)
					cell[i][j]->neighbor[1] = cell[i-1][j];
				else {
					if (neighbor[1] == NULL) cell[i][j]->neighbor[1] = NULL;
					else if (neighbor[1]->cell != NULL) {
						if (j >= neighbor[1]->w_N) cell[i][j]->neighbor[1] = neighbor[1]->cell[neighbor[1]->u_N-1][neighbor[1]->w_N-1]; // allows for possibility that neighbor cell may not be split by the same number of cells

						else cell[i][j]->neighbor[1] = neighbor[1]->cell[neighbor[1]->u_N-1][j];
					} else
						cell[i][j]->neighbor[1] = neighbor[1];
				}

				if (j < w_N-1)
					cell[i][j]->neighbor[2] = cell[i][j+1];
				else {
					if (neighbor[2] == NULL) cell[i][j]->neighbor[2] = NULL;
					else if (neighbor[2]->cell != NULL) {
						if (i >= neighbor[2]->u_N) cell[i][j]->neighbor[2] = neighbor[2]->cell[neighbor[2]->u_N-1][0];
						else cell[i][j]->neighbor[2] = neighbor[2]->cell[i][0];
					} else
						cell[i][j]->neighbor[2] = neighbor[2];
				}

				if (j > 0)
					cell[i][j]->neighbor[3] = cell[i][j-1];
				else {
					if (neighbor[3] == NULL) cell[i][j]->neighbor[3] = NULL;
					else if (neighbor[3]->cell != NULL) {
						if (i >= neighbor[3]->u_N) cell[i][j]->neighbor[3] = neighbor[3]->cell[neighbor[3]->u_N-1][neighbor[3]->w_N-1];
						else cell[i][j]->neighbor[3] = neighbor[3]->cell[i][neighbor[3]->w_N-1];
					} else
						cell[i][j]->neighbor[3] = neighbor[3];
				}
			}
		}
	}
}

void SourcePixelGrid::split_cells(const int usplit, const int wsplit, const int& thread)
{
	if (level >= max_levels+1)
		die("maximum number of splittings has been reached (%i)", max_levels);
	if (cell != NULL)
		die("subcells should not already be present in split_cells routine");

	u_N = usplit;
	w_N = wsplit;
	int i,j;
	for (i=0; i <= u_N; i++) {
		for (j=0; j <= w_N; j++) {
			xvals_threads[thread][i][j][0] = ((corner_pt[0][0]*(w_N-j) + corner_pt[1][0]*j)*(u_N-i) + (corner_pt[2][0]*(w_N-j) + corner_pt[3][0]*j)*i)/(u_N*w_N);
			xvals_threads[thread][i][j][1] = ((corner_pt[0][1]*(w_N-j) + corner_pt[1][1]*j)*(u_N-i) + (corner_pt[2][1]*(w_N-j) + corner_pt[3][1]*j)*i)/(u_N*w_N);
		}
	}

	cell = new SourcePixelGrid**[u_N];
	for (i=0; i < u_N; i++)
	{
		cell[i] = new SourcePixelGrid*[w_N];
		for (j=0; j < w_N; j++) {
			cell[i][j] = new SourcePixelGrid(lens,xvals_threads[thread],i,j,level+1,this);
			cell[i][j]->total_magnification = 0;
			if (lens->n_image_prior) cell[i][j]->n_images = 0;
		}
	}
	if (level == maxlevs[thread]) {
		maxlevs[thread]++; // our subcells are at the max level, so splitting them increases the number of levels by 1
	}
	number_of_pixels += u_N*w_N - 1; // subtract one because we're not counting the parent cell as a source pixel
}

void SourcePixelGrid::unsplit()
{
	if (cell==NULL) return;
	surface_brightness = 0;
	int i,j;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->unsplit();
			surface_brightness += cell[i][j]->surface_brightness;
			delete cell[i][j];
		}
		delete[] cell[i];
	}
	delete[] cell;
	number_of_pixels -= (u_N*w_N - 1);
	cell = NULL;
	surface_brightness /= (u_N*w_N);
	u_N=1; w_N = 1;
}

ofstream SourcePixelGrid::xgrid;

void Lens::plot_source_pixel_grid(const char filename[])
{
	if (source_pixel_grid==NULL) { warn("No source surface brightness map has been generated"); return; }
	SourcePixelGrid::xgrid.open(filename, ifstream::out);
	source_pixel_grid->plot_corner_coordinates();
	SourcePixelGrid::xgrid.close();
}

void SourcePixelGrid::plot_corner_coordinates()
{
	if (level > 0) {
			xgrid << corner_pt[1][0] << " " << corner_pt[1][1] << endl;
			xgrid << corner_pt[3][0] << " " << corner_pt[3][1] << endl;
			xgrid << corner_pt[2][0] << " " << corner_pt[2][1] << endl;
			xgrid << corner_pt[0][0] << " " << corner_pt[0][1] << endl;
			xgrid << corner_pt[1][0] << " " << corner_pt[1][1] << endl;
			xgrid << endl;
	}

	if (cell != NULL)
		for (int i=0; i < u_N; i++)
			for (int j=0; j < w_N; j++)
				cell[i][j]->plot_corner_coordinates();
}

inline bool SourcePixelGrid::check_if_in_neighborhood(lensvector **input_corner_pts, bool& inside, const int& thread)
{
	if (trirec[thread].determine_if_in_neighborhood(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],*input_corner_pts[3],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1],inside)==true) return true;
	return false;
}

inline bool SourcePixelGrid::check_overlap(lensvector **input_corner_pts, const int& thread)
{
	if (trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1])==true) return true;
	if (trirec[thread].determine_if_overlap(*input_corner_pts[1],*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1])==true) return true;
	return false;
}

inline double SourcePixelGrid::find_rectangle_overlap(lensvector **input_corner_pts, const int& thread)
{
	return (trirec[thread].find_overlap_area(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]) + trirec[thread].find_overlap_area(*input_corner_pts[1],*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
}

inline bool SourcePixelGrid::check_triangle1_overlap(lensvector **input_corner_pts, const int& thread)
{
	return trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]);
}

inline bool SourcePixelGrid::check_triangle2_overlap(lensvector **input_corner_pts, const int& thread)
{
	return trirec[thread].determine_if_overlap(*input_corner_pts[1],*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]);
}

inline double SourcePixelGrid::find_triangle1_overlap(lensvector **input_corner_pts, const int& thread)
{
	return (trirec[thread].find_overlap_area(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
}

inline double SourcePixelGrid::find_triangle2_overlap(lensvector **input_corner_pts, const int& thread)
{
	return (trirec[thread].find_overlap_area(*input_corner_pts[1],*input_corner_pts[3],*input_corner_pts[2],corner_pt[0][0],corner_pt[2][0],corner_pt[0][1],corner_pt[1][1]));
}

bool SourcePixelGrid::bisection_search_overlap(lensvector **input_corner_pts, const int& thread)
{
	int i, imid, jmid;
	bool inside;
	bool inside_corner[4];
	int n_inside;
	double xmin[4], xmax[4], ymin[4], ymax[4];
	int reduce_mid = 0;

	for (;;) {
		n_inside=0;
		for (i=0; i < 4; i++) inside_corner[i] = false;
		if (reduce_mid==0) {
			imid = (imax[thread] + imin[thread])/2;
			jmid = (jmax[thread] + jmin[thread])/2;
		} else if (reduce_mid==1) {
			imid = (imax[thread] + 2*imin[thread])/3;
			jmid = (jmax[thread] + 2*jmin[thread])/3;
		} else if (reduce_mid==2) {
			imid = (2*imax[thread] + imin[thread])/3;
			jmid = (2*jmax[thread] + jmin[thread])/3;
		} else if (reduce_mid==3) {
			imid = (imax[thread] + 2*imin[thread])/3;
			jmid = (2*jmax[thread] + jmin[thread])/3;
		} else if (reduce_mid==4) {
			imid = (2*imax[thread] + imin[thread])/3;
			jmid = (jmax[thread] + 2*jmin[thread])/3;
		}
		if ((imid==imin[thread]) or ((imid==imax[thread]))) break;
		if ((jmid==jmin[thread]) or ((jmid==jmax[thread]))) break;
		xmin[0] = cell[imin[thread]][jmin[thread]]->corner_pt[0][0];
		ymin[0] = cell[imin[thread]][jmin[thread]]->corner_pt[0][1];
		xmax[0] = cell[imid][jmid]->corner_pt[3][0];
		ymax[0] = cell[imid][jmid]->corner_pt[3][1];

		xmin[1] = cell[imin[thread]][jmid+1]->corner_pt[0][0];
		ymin[1] = cell[imin[thread]][jmid+1]->corner_pt[0][1];
		xmax[1] = cell[imid][jmax[thread]]->corner_pt[3][0];
		ymax[1] = cell[imid][jmax[thread]]->corner_pt[3][1];

		xmin[2] = cell[imid+1][jmin[thread]]->corner_pt[0][0];
		ymin[2] = cell[imid+1][jmin[thread]]->corner_pt[0][1];
		xmax[2] = cell[imax[thread]][jmid]->corner_pt[3][0];
		ymax[2] = cell[imax[thread]][jmid]->corner_pt[3][1];

		xmin[3] = cell[imid+1][jmid+1]->corner_pt[0][0];
		ymin[3] = cell[imid+1][jmid+1]->corner_pt[0][1];
		xmax[3] = cell[imax[thread]][jmax[thread]]->corner_pt[3][0];
		ymax[3] = cell[imax[thread]][jmax[thread]]->corner_pt[3][1];

		for (i=0; i < 4; i++) {
			inside = false;
			if (trirec[thread].determine_if_in_neighborhood(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],*input_corner_pts[3],xmin[i],xmax[i],ymin[i],ymax[i],inside)) {
				if (inside) inside_corner[i] = true;
				else if (trirec[thread].determine_if_overlap(*input_corner_pts[0],*input_corner_pts[1],*input_corner_pts[2],xmin[i],xmax[i],ymin[i],ymax[i])) inside_corner[i] = true;
				else if (trirec[thread].determine_if_overlap(*input_corner_pts[1],*input_corner_pts[2],*input_corner_pts[3],xmin[i],xmax[i],ymin[i],ymax[i])) inside_corner[i] = true;
				if (inside_corner[i]) n_inside++;
			}
		}
		if (n_inside==0) return false;
		if (n_inside > 1) {
			if (reduce_mid>0) {
				if (reduce_mid < 4) { reduce_mid++; continue; }
				else break; // tried shifting the dividing lines to 1/3 & 2/3 positions, just in case the cell was straddling the middle, but still didn't contain the cell, so give up
			}
			else {
				reduce_mid = 1;
				continue;
			}
		} else if (reduce_mid>0) {
			reduce_mid = 0;
		}

		if (inside_corner[0]) { imax[thread]=imid; jmax[thread]=jmid; }
		else if (inside_corner[1]) { imax[thread]=imid; jmin[thread]=jmid; }
		else if (inside_corner[2]) { imin[thread]=imid; jmax[thread]=jmid; }
		else if (inside_corner[3]) { imin[thread]=imid; jmin[thread]=jmid; }
		if ((imax[thread] - imin[thread] <= 1) or (jmax[thread] - jmin[thread] <= 1)) break;
	}
	return true;
}

void SourcePixelGrid::calculate_pixel_magnifications()
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*(lens->group_comm), *(lens->mpi_group), &sub_comm);
#endif

	int i,j,k,nsrc;
	double overlap_area, weighted_overlap, triangle1_overlap, triangle2_overlap, triangle1_weight, triangle2_weight;
	bool inside;
	clear_subgrids();
	int ntot_src = u_N*w_N;
	double *area_matrix, *mag_matrix;
	mag_matrix = new double[ntot_src];
	if (lens->n_image_prior) {
		area_matrix = new double[ntot_src];
		for (i=0; i < ntot_src; i++) {
			area_matrix[i] = 0;
			mag_matrix[i] = 0;
		}
	} else {
		for (i=0; i < ntot_src; i++) {
			mag_matrix[i] = 0;
		}
	}
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			cell[i][j]->overlap_pixel_n.clear();
		}
	}

#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	double xstep, ystep;
	xstep = (srcgrid_xmax-srcgrid_xmin)/u_N;
	ystep = (srcgrid_ymax-srcgrid_ymin)/w_N;
	int src_raytrace_i, src_raytrace_j;
	int img_i, img_j;

	int ntot = image_pixel_grid->x_N * image_pixel_grid->y_N;
	int *overlap_matrix_row_nn = new int[ntot];
	vector<double> *overlap_matrix_rows = new vector<double>[ntot];
	vector<int> *overlap_matrix_index_rows = new vector<int>[ntot];
	vector<double> *overlap_area_matrix_rows;
	if (lens->n_image_prior) overlap_area_matrix_rows = new vector<double>[ntot];

	int mpi_chunk, mpi_start, mpi_end;
	mpi_chunk = ntot / lens->group_np;
	mpi_start = lens->group_id*mpi_chunk;
	if (lens->group_id == lens->group_np-1) mpi_chunk += (ntot % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end = mpi_start + mpi_chunk;

	int overlap_matrix_nn;
	int overlap_matrix_nn_part=0;
	#pragma omp parallel
	{
		int n, img_i, img_j;
		bool inside;
		int thread;
		int corner_raytrace_i;
		int corner_raytrace_j;
		int min_i, max_i, min_j, max_j;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		#pragma omp for private(i,j,nsrc,overlap_area,weighted_overlap,triangle1_overlap,triangle2_overlap,triangle1_weight,triangle2_weight,inside) schedule(dynamic) reduction(+:overlap_matrix_nn_part)
		for (n=mpi_start; n < mpi_end; n++)
		{
			overlap_matrix_row_nn[n] = 0;
			img_j = n / image_pixel_grid->x_N;
			img_i = n % image_pixel_grid->x_N;

			corners_threads[thread][0] = &image_pixel_grid->corner_sourcepts[img_i][img_j];
			corners_threads[thread][1] = &image_pixel_grid->corner_sourcepts[img_i][img_j+1];
			corners_threads[thread][2] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j];
			corners_threads[thread][3] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j+1];

			min_i = (int) (((*corners_threads[thread][0])[0] - srcgrid_xmin) / xstep);
			min_j = (int) (((*corners_threads[thread][0])[1] - srcgrid_ymin) / ystep);
			max_i = min_i;
			max_j = min_j;
			for (i=1; i < 4; i++) {
				corner_raytrace_i = (int) (((*corners_threads[thread][i])[0] - srcgrid_xmin) / xstep);
				corner_raytrace_j = (int) (((*corners_threads[thread][i])[1] - srcgrid_ymin) / ystep);
				if (corner_raytrace_i < min_i) min_i = corner_raytrace_i;
				if (corner_raytrace_i > max_i) max_i = corner_raytrace_i;
				if (corner_raytrace_j < min_j) min_j = corner_raytrace_j;
				if (corner_raytrace_j > max_j) max_j = corner_raytrace_j;
			}
			if ((min_i < 0) or (min_i >= u_N)) continue;
			if ((min_j < 0) or (min_j >= w_N)) continue;
			if ((max_i < 0) or (max_i >= u_N)) continue;
			if ((max_j < 0) or (max_j >= w_N)) continue;

			for (j=min_j; j <= max_j; j++) {
				for (i=min_i; i <= max_i; i++) {
					nsrc = j*u_N + i;
					if (cell[i][j]->check_if_in_neighborhood(corners_threads[thread],inside,thread)) {
						if (inside) {
							triangle1_overlap = cell[i][j]->find_triangle1_overlap(corners_threads[thread],thread);
							triangle2_overlap = cell[i][j]->find_triangle2_overlap(corners_threads[thread],thread);
							triangle1_weight = triangle1_overlap / image_pixel_grid->source_plane_triangle1_area[img_i][img_j];
							triangle2_weight = triangle2_overlap / image_pixel_grid->source_plane_triangle2_area[img_i][img_j];
						} else {
							if (cell[i][j]->check_triangle1_overlap(corners_threads[thread],thread)) {
								triangle1_overlap = cell[i][j]->find_triangle1_overlap(corners_threads[thread],thread);
								triangle1_weight = triangle1_overlap / image_pixel_grid->source_plane_triangle1_area[img_i][img_j];
							} else {
								triangle1_overlap = 0;
								triangle1_weight = 0;
							}
							if (cell[i][j]->check_triangle2_overlap(corners_threads[thread],thread)) {
								triangle2_overlap = cell[i][j]->find_triangle2_overlap(corners_threads[thread],thread);
								triangle2_weight = triangle2_overlap / image_pixel_grid->source_plane_triangle2_area[img_i][img_j];
							} else {
								triangle2_overlap = 0;
								triangle2_weight = 0;
							}
						}
						if ((triangle1_overlap != 0) or (triangle2_overlap != 0)) {
							weighted_overlap = triangle1_weight + triangle2_weight;
							overlap_matrix_rows[n].push_back(weighted_overlap);
							overlap_matrix_index_rows[n].push_back(nsrc);
							overlap_matrix_row_nn[n]++;

							if (lens->n_image_prior) {
								overlap_area = triangle1_overlap + triangle2_overlap;
								overlap_area_matrix_rows[n].push_back(overlap_area);
							}

						}
					}
				}
			}
			overlap_matrix_nn_part += overlap_matrix_row_nn[n];
		}
	}

#ifdef USE_MPI
	MPI_Allreduce(&overlap_matrix_nn_part, &overlap_matrix_nn, 1, MPI_INT, MPI_SUM, sub_comm);
#else
	overlap_matrix_nn = overlap_matrix_nn_part;
#endif

	double *overlap_matrix = new double[overlap_matrix_nn];
	int *overlap_matrix_index = new int[overlap_matrix_nn];
	int *image_pixel_location_overlap = new int[ntot+1];
	double *overlap_area_matrix;
	if (lens->n_image_prior) overlap_area_matrix = new double[overlap_matrix_nn];

#ifdef USE_MPI
	int id, chunk, start, end, length;
	for (id=0; id < lens->group_np; id++) {
		chunk = ntot / lens->group_np;
		start = id*chunk;
		if (id == lens->group_np-1) chunk += (ntot % lens->group_np); // assign the remainder elements to the last mpi process
		MPI_Bcast(overlap_matrix_row_nn + start,chunk,MPI_INT,id,sub_comm);
	}
#endif

	image_pixel_location_overlap[0] = 0;
	int n,l;
	for (n=0; n < ntot; n++) {
		image_pixel_location_overlap[n+1] = image_pixel_location_overlap[n] + overlap_matrix_row_nn[n];
	}

	int indx;
	for (n=mpi_start; n < mpi_end; n++) {
		indx = image_pixel_location_overlap[n];
		for (j=0; j < overlap_matrix_row_nn[n]; j++) {
			overlap_matrix[indx+j] = overlap_matrix_rows[n][j];
			overlap_matrix_index[indx+j] = overlap_matrix_index_rows[n][j];
			if (lens->n_image_prior) overlap_area_matrix[indx+j] = overlap_area_matrix_rows[n][j];
		}
	}

#ifdef USE_MPI
	for (id=0; id < lens->group_np; id++) {
		chunk = ntot / lens->group_np;
		start = id*chunk;
		if (id == lens->group_np-1) chunk += (ntot % lens->group_np); // assign the remainder elements to the last mpi process
		end = start + chunk;
		length = image_pixel_location_overlap[end] - image_pixel_location_overlap[start];
		MPI_Bcast(overlap_matrix + image_pixel_location_overlap[start],length,MPI_DOUBLE,id,sub_comm);
		MPI_Bcast(overlap_matrix_index + image_pixel_location_overlap[start],length,MPI_INT,id,sub_comm);
		if (lens->n_image_prior)
			MPI_Bcast(overlap_area_matrix + image_pixel_location_overlap[start],length,MPI_DOUBLE,id,sub_comm);
	}
	MPI_Comm_free(&sub_comm);
#endif

	for (n=0; n < ntot; n++) {
		img_j = n / image_pixel_grid->x_N;
		img_i = n % image_pixel_grid->x_N;
		for (l=image_pixel_location_overlap[n]; l < image_pixel_location_overlap[n+1]; l++) {
			nsrc = overlap_matrix_index[l];
			j = nsrc / u_N;
			i = nsrc % u_N;
			mag_matrix[nsrc] += overlap_matrix[l];
			if (lens->n_image_prior) area_matrix[nsrc] += overlap_area_matrix[l];
			cell[i][j]->overlap_pixel_n.push_back(n);
			if ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[img_i][img_j]==true)) cell[i][j]->maps_to_image_window = true;
		}
	}

#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for finding source cell magnifications: " << wtime << endl;
	}
#endif

	//for (j=0; j < w_N; j++) {
		//for (i=0; i < u_N; i++) {
			//overlap_area=0;
			//weighted_overlap=0;
			//for (k=0; k < cell[i][j]->overlaps.size(); k++) {
				//overlap_area += cell[i][j]->overlaps[k];
				//weighted_overlap += cell[i][j]->weighted_overlaps[k];
			//}
			//cell[i][j]->total_magnification = weighted_overlap * image_pixel_grid->triangle_area / cell[i][j]->cell_area;
			//cell[i][j]->total_magnification *= image_pixel_grid->triangle_area / cell[i][j]->cell_area;
			//if (cell[i][j]->total_magnification*0.0) warn("Nonsensical source cell magnification (mag=%g",cell[i][j]->total_magnification);
			//cell[i][j]->n_images = overlap_area / cell[i][j]->cell_area;
		//}
	//}
	for (nsrc=0; nsrc < ntot_src; nsrc++) {
		j = nsrc / u_N;
		i = nsrc % u_N;
		cell[i][j]->total_magnification = mag_matrix[nsrc] * image_pixel_grid->triangle_area / cell[i][j]->cell_area;
		if (lens->n_image_prior) cell[i][j]->n_images = area_matrix[nsrc] / cell[i][j]->cell_area;
		//cout << mag_matrix[nsrc] << " " << cell[i][j]->total_magnification << endl;
		if (cell[i][j]->total_magnification*0.0) warn("Nonsensical source cell magnification (mag=%g",cell[i][j]->total_magnification);
	}

	delete[] overlap_matrix;
	delete[] overlap_matrix_index;
	delete[] image_pixel_location_overlap;
	delete[] overlap_matrix_rows;
	delete[] overlap_matrix_index_rows;
	delete[] overlap_matrix_row_nn;
	delete[] mag_matrix;
	if (lens->n_image_prior) {
		delete[] overlap_area_matrix;
		delete[] overlap_area_matrix_rows;
		delete[] area_matrix;
	}
}

void SourcePixelGrid::adaptive_subgrid()
{
	calculate_pixel_magnifications();
#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int i, prev_levels;
	for (i=0; i < max_levels-1; i++) {
		prev_levels = levels;
		split_subcells_firstlevel(i);
		if (prev_levels==levels) break; // no splitting occurred, so no need to attempt further subgridding
	}
	assign_all_neighbors();

#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for adaptive grid splitting: " << wtime << endl;
	}
#endif
}

void SourcePixelGrid::split_subcells_firstlevel(const int splitlevel)
{
	if (level >= max_levels+1)
		die("maximum number of splittings has been reached (%i)", max_levels);

	int ntot = u_N*w_N;
	int i,j,n;
	if (splitlevel > level) {
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			maxlevs[thread] = levels;
			#pragma omp for private(i,j,n) schedule(dynamic)
			for (n=0; n < ntot; n++) {
				j = n / u_N;
				i = n % u_N;
				if (cell[i][j]->cell != NULL) cell[i][j]->split_subcells(splitlevel,thread);
			}
		}
		for (i=0; i < nthreads; i++) if (maxlevs[i] > levels) levels = maxlevs[i];
	} else {
		int k,l,m;
		double overlap_area, weighted_overlap, triangle1_overlap, triangle2_overlap, triangle1_weight, triangle2_weight;
		double mag_threshold = 4*lens->pixel_magnification_threshold;
		SourcePixelGrid *subcell;
		if (level > 0) {
			for (i=0; i < level; i++) {
				mag_threshold *= 4;
			}
		}
		bool subgrid;
		#pragma omp parallel
		{
			int nn, img_i, img_j;
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			maxlevs[thread] = levels;
			double xstep, ystep;
			xstep = (srcgrid_xmax-srcgrid_xmin)/u_N/2.0;
			ystep = (srcgrid_ymax-srcgrid_ymin)/w_N/2.0;
			int min_i,max_i,min_j,max_j;
			int corner_raytrace_i, corner_raytrace_j;
			int ii,lmin,lmax,mmin,mmax;

			#pragma omp for private(i,j,n,k,l,m,overlap_area,weighted_overlap,triangle1_overlap,triangle2_overlap,triangle1_weight,triangle2_weight,subgrid,subcell) schedule(dynamic)
			for (n=0; n < ntot; n++) {
				j = n / u_N;
				i = n % u_N;
				subgrid = false;
				if (cell[i][j]->total_magnification > mag_threshold) subgrid = true;
				if (subgrid) {
					cell[i][j]->split_cells(2,2,thread);
					for (k=0; k < cell[i][j]->overlap_pixel_n.size(); k++) {
						nn = cell[i][j]->overlap_pixel_n[k];
						img_j = nn / image_pixel_grid->x_N;
						img_i = nn % image_pixel_grid->x_N;
						corners_threads[thread][0] = &image_pixel_grid->corner_sourcepts[img_i][img_j];
						corners_threads[thread][1] = &image_pixel_grid->corner_sourcepts[img_i][img_j+1];
						corners_threads[thread][2] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j];
						corners_threads[thread][3] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j+1];

						min_i = (int) (((*corners_threads[thread][0])[0] - cell[i][j]->corner_pt[0][0]) / xstep);
						min_j = (int) (((*corners_threads[thread][0])[1] - cell[i][j]->corner_pt[0][1]) / ystep);
						max_i = min_i;
						max_j = min_j;
						for (ii=1; ii < 4; ii++) {
							corner_raytrace_i = (int) (((*corners_threads[thread][ii])[0] - cell[i][j]->corner_pt[0][0]) / xstep);
							corner_raytrace_j = (int) (((*corners_threads[thread][ii])[1] - cell[i][j]->corner_pt[0][1]) / ystep);
							if (corner_raytrace_i < min_i) min_i = corner_raytrace_i;
							if (corner_raytrace_i > max_i) max_i = corner_raytrace_i;
							if (corner_raytrace_j < min_j) min_j = corner_raytrace_j;
							if (corner_raytrace_j > max_j) max_j = corner_raytrace_j;
						}
						lmin=0;
						lmax=cell[i][j]->u_N-1;
						mmin=0;
						mmax=cell[i][j]->w_N-1;
						if ((min_i >= 0) and (min_i < cell[i][j]->u_N)) lmin = min_i;
						if ((max_i >= 0) and (max_i < cell[i][j]->u_N)) lmax = max_i;
						if ((min_j >= 0) and (min_j < cell[i][j]->w_N)) mmin = min_j;
						if ((max_j >= 0) and (max_j < cell[i][j]->w_N)) mmax = max_j;

						for (l=lmin; l <= lmax; l++) {
							for (m=mmin; m <= mmax; m++) {
								subcell = cell[i][j]->cell[l][m];
								triangle1_overlap = subcell->find_triangle1_overlap(corners_threads[thread],thread);
								triangle2_overlap = subcell->find_triangle2_overlap(corners_threads[thread],thread);
								triangle1_weight = triangle1_overlap / image_pixel_grid->source_plane_triangle1_area[img_i][img_j];
								triangle2_weight = triangle2_overlap / image_pixel_grid->source_plane_triangle2_area[img_i][img_j];
								weighted_overlap = triangle1_weight + triangle2_weight;

								subcell->total_magnification += weighted_overlap;
								if ((weighted_overlap != 0) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[img_i][img_j]==true))) subcell->maps_to_image_window = true;
								subcell->overlap_pixel_n.push_back(nn);
								if (lens->n_image_prior) {
									overlap_area = triangle1_overlap + triangle2_overlap;
									subcell->n_images += overlap_area;
								}
							}
						}
					}
					for (l=0; l < cell[i][j]->u_N; l++) {
						for (m=0; m < cell[i][j]->w_N; m++) {
							subcell = cell[i][j]->cell[l][m];
							subcell->total_magnification *= image_pixel_grid->triangle_area / subcell->cell_area;
							if (lens->n_image_prior) subcell->n_images /= subcell->cell_area;
						}
					}
				}
			}
		}
		for (i=0; i < nthreads; i++) if (maxlevs[i] > levels) levels = maxlevs[i];
	}
}

void SourcePixelGrid::split_subcells(const int splitlevel, const int thread)
{
	if (level >= max_levels+1)
		die("maximum number of splittings has been reached (%i)", max_levels);

	int i,j;
	if (splitlevel > level) {
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				if (cell[i][j]->cell != NULL) cell[i][j]->split_subcells(splitlevel,thread);
			}
		}
	} else {
		double xstep, ystep;
		xstep = (corner_pt[2][0] - corner_pt[0][0])/u_N/2.0;
		ystep = (corner_pt[1][1] - corner_pt[0][1])/w_N/2.0;
		int min_i,max_i,min_j,max_j;
		int corner_raytrace_i, corner_raytrace_j;
		int ii,lmin,lmax,mmin,mmax;

		int k,l,m,nn,img_i,img_j;
		double overlap_area, weighted_overlap, triangle1_overlap, triangle2_overlap, triangle1_weight, triangle2_weight;
		double mag_threshold = 4*lens->pixel_magnification_threshold;
		SourcePixelGrid *subcell;
		if (level > 0) {
			for (i=0; i < level; i++) {
				mag_threshold *= 4;
			}
		}
		bool subgrid;
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				subgrid = false;
				if (cell[i][j]->total_magnification > mag_threshold) subgrid = true;
				if (subgrid) {
					cell[i][j]->split_cells(2,2,thread);
					for (k=0; k < cell[i][j]->overlap_pixel_n.size(); k++) {
						nn = cell[i][j]->overlap_pixel_n[k];
						img_j = nn / image_pixel_grid->x_N;
						img_i = nn % image_pixel_grid->x_N;
						corners_threads[thread][0] = &image_pixel_grid->corner_sourcepts[img_i][img_j];
						corners_threads[thread][1] = &image_pixel_grid->corner_sourcepts[img_i][img_j+1];
						corners_threads[thread][2] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j];
						corners_threads[thread][3] = &image_pixel_grid->corner_sourcepts[img_i+1][img_j+1];

						min_i = (int) (((*corners_threads[thread][0])[0] - cell[i][j]->corner_pt[0][0]) / xstep);
						min_j = (int) (((*corners_threads[thread][0])[1] - cell[i][j]->corner_pt[0][1]) / ystep);
						max_i = min_i;
						max_j = min_j;
						for (ii=1; ii < 4; ii++) {
							corner_raytrace_i = (int) (((*corners_threads[thread][ii])[0] - cell[i][j]->corner_pt[0][0]) / xstep);
							corner_raytrace_j = (int) (((*corners_threads[thread][ii])[1] - cell[i][j]->corner_pt[0][1]) / ystep);
							if (corner_raytrace_i < min_i) min_i = corner_raytrace_i;
							if (corner_raytrace_i > max_i) max_i = corner_raytrace_i;
							if (corner_raytrace_j < min_j) min_j = corner_raytrace_j;
							if (corner_raytrace_j > max_j) max_j = corner_raytrace_j;
						}
						lmin=0;
						lmax=cell[i][j]->u_N-1;
						mmin=0;
						mmax=cell[i][j]->w_N-1;
						if ((min_i >= 0) and (min_i < cell[i][j]->u_N)) lmin = min_i;
						if ((max_i >= 0) and (max_i < cell[i][j]->u_N)) lmax = max_i;
						if ((min_j >= 0) and (min_j < cell[i][j]->w_N)) mmin = min_j;
						if ((max_j >= 0) and (max_j < cell[i][j]->w_N)) mmax = max_j;

						for (l=lmin; l <= lmax; l++) {
							for (m=mmin; m <= mmax; m++) {
								subcell = cell[i][j]->cell[l][m];
								triangle1_overlap = subcell->find_triangle1_overlap(corners_threads[thread],thread);
								triangle2_overlap = subcell->find_triangle2_overlap(corners_threads[thread],thread);
								triangle1_weight = triangle1_overlap / image_pixel_grid->source_plane_triangle1_area[img_i][img_j];
								triangle2_weight = triangle2_overlap / image_pixel_grid->source_plane_triangle2_area[img_i][img_j];
								weighted_overlap = triangle1_weight + triangle2_weight;

								subcell->total_magnification += weighted_overlap;
								subcell->overlap_pixel_n.push_back(nn);
								if ((weighted_overlap != 0) and ((image_pixel_grid->fit_to_data==NULL) or (image_pixel_grid->fit_to_data[img_i][img_j]==true))) subcell->maps_to_image_window = true;
								if (lens->n_image_prior) {
									overlap_area = triangle1_overlap + triangle2_overlap;
									subcell->n_images += overlap_area;
								}
							}
						}
					}
					for (l=0; l < cell[i][j]->u_N; l++) {
						for (m=0; m < cell[i][j]->w_N; m++) {
							subcell = cell[i][j]->cell[l][m];
							subcell->total_magnification *= image_pixel_grid->triangle_area / subcell->cell_area;
							if (lens->n_image_prior) subcell->n_images /= subcell->cell_area;
						}
					}
				}
			}
		}
	}
}

bool SourcePixelGrid::assign_source_mapping_flags_overlap(lensvector **input_corner_pts, vector<SourcePixelGrid*>& mapped_source_pixels, const int& thread)
{
	imin[thread]=0; imax[thread]=u_N-1;
	jmin[thread]=0; jmax[thread]=w_N-1;
	if (bisection_search_overlap(input_corner_pts,thread)==false) return false;

	bool image_pixel_maps_to_source_grid = false;
	bool inside;
	int i,j;
	for (j=jmin[thread]; j <= jmax[thread]; j++) {
		for (i=imin[thread]; i <= imax[thread]; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->subcell_assign_source_mapping_flags_overlap(input_corner_pts,mapped_source_pixels,thread,image_pixel_maps_to_source_grid);
			else {
				if (!cell[i][j]->check_if_in_neighborhood(input_corner_pts,inside,thread)) continue;
				if ((inside) or (cell[i][j]->check_overlap(input_corner_pts,thread))) {
					cell[i][j]->maps_to_image_pixel = true;
					mapped_source_pixels.push_back(cell[i][j]);
					if (!image_pixel_maps_to_source_grid) image_pixel_maps_to_source_grid = true;
				}
			}
		}
	}
	return image_pixel_maps_to_source_grid;
}

void SourcePixelGrid::subcell_assign_source_mapping_flags_overlap(lensvector **input_corner_pts, vector<SourcePixelGrid*>& mapped_source_pixels, const int& thread, bool& image_pixel_maps_to_source_grid)
{
	bool inside;
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->subcell_assign_source_mapping_flags_overlap(input_corner_pts,mapped_source_pixels,thread,image_pixel_maps_to_source_grid);
			else {
				if (!cell[i][j]->check_if_in_neighborhood(input_corner_pts,inside,thread)) continue;
				if ((inside) or (cell[i][j]->check_overlap(input_corner_pts,thread))) {
					cell[i][j]->maps_to_image_pixel = true;
					mapped_source_pixels.push_back(cell[i][j]);
					if (!image_pixel_maps_to_source_grid) image_pixel_maps_to_source_grid = true;
				}
			}
		}
	}
}

void SourcePixelGrid::calculate_Lmatrix_overlap(const int &img_index, const int &image_pixel_i, const int &image_pixel_j, int& index, lensvector **input_corner_pts, const int& thread)
{
	double overlap, total_overlap=0;
	int i,j,k;
	int Lmatrix_index_initial = index;
	SourcePixelGrid *subcell;

	for (i=0; i < image_pixel_grid->mapped_source_pixels[image_pixel_i][image_pixel_j].size(); i++) {
		subcell = image_pixel_grid->mapped_source_pixels[image_pixel_i][image_pixel_j][i];
		lens->Lmatrix_index_rows[img_index].push_back(subcell->active_index);
		overlap = subcell->find_rectangle_overlap(input_corner_pts,thread);
		lens->Lmatrix_rows[img_index].push_back(overlap);
		index++;
		total_overlap += overlap;
	}

	if (total_overlap==0) die("image pixel should have mapped to at least one source pixel");
	for (i=Lmatrix_index_initial; i < index; i++)
		lens->Lmatrix_rows[img_index][i] /= total_overlap;
}

double SourcePixelGrid::find_lensed_surface_brightness_overlap(lensvector **input_corner_pts, const int& thread)
{
	imin[thread]=0; imax[thread]=u_N-1;
	jmin[thread]=0; jmax[thread]=w_N-1;
	if (bisection_search_overlap(input_corner_pts,thread)==false) return false;

	double total_overlap = 0;
	double total_weighted_surface_brightness = 0;
	double overlap;
	int i,j;
	for (j=jmin[thread]; j <= jmax[thread]; j++) {
		for (i=imin[thread]; i <= imax[thread]; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->find_lensed_surface_brightness_subcell_overlap(input_corner_pts,thread,overlap,total_overlap,total_weighted_surface_brightness);
			else {
				overlap = cell[i][j]->find_rectangle_overlap(input_corner_pts,thread);
				total_overlap += overlap;
				total_weighted_surface_brightness += overlap*cell[i][j]->surface_brightness;
			}
		}
	}
	double lensed_surface_brightness;
	if (total_overlap==0) lensed_surface_brightness = 0;
	else lensed_surface_brightness = total_weighted_surface_brightness/total_overlap;
	return lensed_surface_brightness;
}

void SourcePixelGrid::find_lensed_surface_brightness_subcell_overlap(lensvector **input_corner_pts, const int& thread, double& overlap, double& total_overlap, double& total_weighted_surface_brightness)
{
	int i,j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->find_lensed_surface_brightness_subcell_overlap(input_corner_pts,thread,overlap,total_overlap,total_weighted_surface_brightness);
			else {
				overlap = cell[i][j]->find_rectangle_overlap(input_corner_pts,thread);
				total_overlap += overlap;
				total_weighted_surface_brightness += overlap*cell[i][j]->surface_brightness;
			}
		}
	}
}

bool SourcePixelGrid::bisection_search_interpolate(lensvector &input_center_pt, const int& thread)
{
	int i, imid, jmid;
	bool inside;
	bool inside_corner[4];
	int n_inside;
	double xmin[4], xmax[4], ymin[4], ymax[4];

	for (;;) {
		n_inside=0;
		for (i=0; i < 4; i++) inside_corner[i] = false;
		imid = (imax[thread] + imin[thread])/2;
		jmid = (jmax[thread] + jmin[thread])/2;
		xmin[0] = cell[imin[thread]][jmin[thread]]->corner_pt[0][0];
		ymin[0] = cell[imin[thread]][jmin[thread]]->corner_pt[0][1];
		xmax[0] = cell[imid][jmid]->corner_pt[3][0];
		ymax[0] = cell[imid][jmid]->corner_pt[3][1];

		xmin[1] = cell[imin[thread]][jmid+1]->corner_pt[0][0];
		ymin[1] = cell[imin[thread]][jmid+1]->corner_pt[0][1];
		xmax[1] = cell[imid][jmax[thread]]->corner_pt[3][0];
		ymax[1] = cell[imid][jmax[thread]]->corner_pt[3][1];

		xmin[2] = cell[imid+1][jmin[thread]]->corner_pt[0][0];
		ymin[2] = cell[imid+1][jmin[thread]]->corner_pt[0][1];
		xmax[2] = cell[imax[thread]][jmid]->corner_pt[3][0];
		ymax[2] = cell[imax[thread]][jmid]->corner_pt[3][1];

		xmin[3] = cell[imid+1][jmid+1]->corner_pt[0][0];
		ymin[3] = cell[imid+1][jmid+1]->corner_pt[0][1];
		xmax[3] = cell[imax[thread]][jmax[thread]]->corner_pt[3][0];
		ymax[3] = cell[imax[thread]][jmax[thread]]->corner_pt[3][1];

		for (i=0; i < 4; i++) {
			if ((input_center_pt[0] >= xmin[i]) and (input_center_pt[0] < xmax[i]) and (input_center_pt[1] >= ymin[i]) and (input_center_pt[1] < ymax[i])) {
				inside_corner[i] = true;
				n_inside++;
			}
		}
		if (n_inside==0) return false;
		if (n_inside > 1) die("should not be inside more than one rectangle");
		else {
			if (inside_corner[0]) { imax[thread]=imid; jmax[thread]=jmid; }
			else if (inside_corner[1]) { imax[thread]=imid; jmin[thread]=jmid; }
			else if (inside_corner[2]) { imin[thread]=imid; jmax[thread]=jmid; }
			else if (inside_corner[3]) { imin[thread]=imid; jmin[thread]=jmid; }
		}
		if ((imax[thread] - imin[thread] <= 1) or (jmax[thread] - jmin[thread] <= 1)) break;
	}
	return true;
}

bool SourcePixelGrid::assign_source_mapping_flags_interpolate(lensvector &input_center_pt, vector<SourcePixelGrid*>& mapped_source_pixels, const int& thread, const int& image_pixel_i, const int& image_pixel_j)
{
	imin[thread]=0; imax[thread]=u_N-1;
	jmin[thread]=0; jmax[thread]=w_N-1;
	if (bisection_search_interpolate(input_center_pt,thread)==false) return false;

	bool image_pixel_maps_to_source_grid = false;
	int i,j,side;
	SourcePixelGrid* cellptr;
	for (j=jmin[thread]; j <= jmax[thread]; j++) {
		for (i=imin[thread]; i <= imax[thread]; i++) {
			if ((input_center_pt[0] >= cell[i][j]->corner_pt[0][0]) and (input_center_pt[0] < cell[i][j]->corner_pt[2][0]) and (input_center_pt[1] >= cell[i][j]->corner_pt[0][1]) and (input_center_pt[1] < cell[i][j]->corner_pt[3][1])) {
				if (cell[i][j]->cell != NULL) image_pixel_maps_to_source_grid = cell[i][j]->subcell_assign_source_mapping_flags_interpolate(input_center_pt,mapped_source_pixels,thread);
				else {
					cell[i][j]->maps_to_image_pixel = true;
					mapped_source_pixels.push_back(cell[i][j]);
					if (!image_pixel_maps_to_source_grid) image_pixel_maps_to_source_grid = true;
					if (((input_center_pt[0] > cell[i][j]->center_pt[0]) and (cell[i][j]->neighbor[0] != NULL)) or (cell[i][j]->neighbor[1] == NULL)) {
						if (cell[i][j]->neighbor[0]->cell != NULL) {
							side=0;
							cellptr = cell[i][j]->neighbor[0]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[0]->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cell[i][j]->neighbor[0]);
						}
					} else {
						if (cell[i][j]->neighbor[1]->cell != NULL) {
							side=1;
							cellptr = cell[i][j]->neighbor[1]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[1]->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cell[i][j]->neighbor[1]);
						}
					}
					if (((input_center_pt[1] > cell[i][j]->center_pt[1]) and (cell[i][j]->neighbor[2] != NULL)) or (cell[i][j]->neighbor[3] == NULL)) {
						if (cell[i][j]->neighbor[2]->cell != NULL) {
							side=2;
							cellptr = cell[i][j]->neighbor[2]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[2]->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cell[i][j]->neighbor[2]);
						}
					} else {
						if (cell[i][j]->neighbor[3]->cell != NULL) {
							side=3;
							cellptr = cell[i][j]->neighbor[3]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[3]->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cell[i][j]->neighbor[3]);
						}
					}
				}
				break;
			}
		}
	}
	if (mapped_source_pixels.size() != 3) die("Did not assign enough interpolation cells!");

	return image_pixel_maps_to_source_grid;
}

bool SourcePixelGrid::subcell_assign_source_mapping_flags_interpolate(lensvector &input_center_pt, vector<SourcePixelGrid*>& mapped_source_pixels, const int& thread)
{
	bool image_pixel_maps_to_source_grid = false;
	int i,j,side;
	SourcePixelGrid* cellptr;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if ((input_center_pt[0] >= cell[i][j]->corner_pt[0][0]) and (input_center_pt[0] < cell[i][j]->corner_pt[2][0]) and (input_center_pt[1] >= cell[i][j]->corner_pt[0][1]) and (input_center_pt[1] < cell[i][j]->corner_pt[3][1])) {
				if (cell[i][j]->cell != NULL) image_pixel_maps_to_source_grid = cell[i][j]->subcell_assign_source_mapping_flags_interpolate(input_center_pt,mapped_source_pixels,thread);
				else {
					cell[i][j]->maps_to_image_pixel = true;
					mapped_source_pixels.push_back(cell[i][j]);
					if (!image_pixel_maps_to_source_grid) image_pixel_maps_to_source_grid = true;
					if (((input_center_pt[0] > cell[i][j]->center_pt[0]) and (cell[i][j]->neighbor[0] != NULL)) or (cell[i][j]->neighbor[1] == NULL)) {
						if (cell[i][j]->neighbor[0]->cell != NULL) {
							side=0;
							cellptr = cell[i][j]->neighbor[0]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[0]->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cell[i][j]->neighbor[0]);
						}
					} else {
						if (cell[i][j]->neighbor[1]->cell != NULL) {
							side=1;
							cellptr = cell[i][j]->neighbor[1]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[1]->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cell[i][j]->neighbor[1]);
						}
					}
					if (((input_center_pt[1] > cell[i][j]->center_pt[1]) and (cell[i][j]->neighbor[2] != NULL)) or (cell[i][j]->neighbor[3] == NULL)) {
						if (cell[i][j]->neighbor[2]->cell != NULL) {
							side=2;
							cellptr = cell[i][j]->neighbor[2]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[2]->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cell[i][j]->neighbor[2]);
						}
					} else {
						if (cell[i][j]->neighbor[3]->cell != NULL) {
							side=3;
							cellptr = cell[i][j]->neighbor[3]->find_nearest_neighbor_cell(input_center_pt,side);
							cellptr->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cellptr);
						}
						else {
							cell[i][j]->neighbor[3]->maps_to_image_pixel = true;
							mapped_source_pixels.push_back(cell[i][j]->neighbor[3]);
						}
					}
				}
				break;
			}
		}
	}
	return image_pixel_maps_to_source_grid;
}

void SourcePixelGrid::calculate_Lmatrix_interpolate(const int img_index, const int image_pixel_i, const int image_pixel_j, int& index, lensvector &input_center_pt, const int& thread)
{
	for (int i=0; i < 3; i++) {
		lens->Lmatrix_index_rows[img_index].push_back(image_pixel_grid->mapped_source_pixels[image_pixel_i][image_pixel_j][i]->active_index);
		interpolation_pts[i][thread] = &image_pixel_grid->mapped_source_pixels[image_pixel_i][image_pixel_j][i]->center_pt;
	}

	double d = ((*interpolation_pts[0][thread])[0]-(*interpolation_pts[1][thread])[0])*((*interpolation_pts[1][thread])[1]-(*interpolation_pts[2][thread])[1]) - ((*interpolation_pts[1][thread])[0]-(*interpolation_pts[2][thread])[0])*((*interpolation_pts[0][thread])[1]-(*interpolation_pts[1][thread])[1]);
	lens->Lmatrix_rows[img_index].push_back((input_center_pt[0]*((*interpolation_pts[1][thread])[1]-(*interpolation_pts[2][thread])[1]) + input_center_pt[1]*((*interpolation_pts[2][thread])[0]-(*interpolation_pts[1][thread])[0]) + (*interpolation_pts[1][thread])[0]*(*interpolation_pts[2][thread])[1] - (*interpolation_pts[1][thread])[1]*(*interpolation_pts[2][thread])[0])/d);
	lens->Lmatrix_rows[img_index].push_back((input_center_pt[0]*((*interpolation_pts[2][thread])[1]-(*interpolation_pts[0][thread])[1]) + input_center_pt[1]*((*interpolation_pts[0][thread])[0]-(*interpolation_pts[2][thread])[0]) + (*interpolation_pts[0][thread])[1]*(*interpolation_pts[2][thread])[0] - (*interpolation_pts[0][thread])[0]*(*interpolation_pts[2][thread])[1])/d);
	lens->Lmatrix_rows[img_index].push_back((input_center_pt[0]*((*interpolation_pts[0][thread])[1]-(*interpolation_pts[1][thread])[1]) + input_center_pt[1]*((*interpolation_pts[1][thread])[0]-(*interpolation_pts[0][thread])[0]) + (*interpolation_pts[0][thread])[0]*(*interpolation_pts[1][thread])[1] - (*interpolation_pts[0][thread])[1]*(*interpolation_pts[1][thread])[0])/d);

	index += 3;
}

double SourcePixelGrid::find_lensed_surface_brightness_interpolate(lensvector &input_center_pt, const int& thread)
{
	lensvector *pts[3];
	double *sb[3];
	int indx=0;
	nearest_interpolation_cells[thread].found_containing_cell = false;
	for (int i=0; i < 3; i++) nearest_interpolation_cells[thread].pixel[i] = NULL;

	imin[thread]=0; imax[thread]=u_N-1;
	jmin[thread]=0; jmax[thread]=w_N-1;
	if (bisection_search_interpolate(input_center_pt,thread)==false) return false;

	bool image_pixel_maps_to_source_grid = false;
	int i,j,side;
	for (j=jmin[thread]; j <= jmax[thread]; j++) {
		for (i=imin[thread]; i <= imax[thread]; i++) {
			if ((input_center_pt[0] >= cell[i][j]->corner_pt[0][0]) and (input_center_pt[0] < cell[i][j]->corner_pt[2][0]) and (input_center_pt[1] >= cell[i][j]->corner_pt[0][1]) and (input_center_pt[1] < cell[i][j]->corner_pt[3][1])) {
				if (cell[i][j]->cell != NULL) cell[i][j]->find_interpolation_cells(input_center_pt,thread);
				else {
					nearest_interpolation_cells[thread].found_containing_cell = true;
					nearest_interpolation_cells[thread].pixel[0] = cell[i][j];
					if (((input_center_pt[0] > cell[i][j]->center_pt[0]) and (cell[i][j]->neighbor[0] != NULL)) or (cell[i][j]->neighbor[1] == NULL)) {
						if (cell[i][j]->neighbor[0]->cell != NULL) {
							side=0;
							nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[0]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[0];
					} else {
						if (cell[i][j]->neighbor[1]->cell != NULL) {
							side=1;
							nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[1]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[1];
					}
					if (((input_center_pt[1] > cell[i][j]->center_pt[1]) and (cell[i][j]->neighbor[2] != NULL)) or (cell[i][j]->neighbor[3] == NULL)) {
						if (cell[i][j]->neighbor[2]->cell != NULL) {
							side=2;
							nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[2]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[2];
					} else {
						if (cell[i][j]->neighbor[3]->cell != NULL) {
							side=3;
							nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[3]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[3];
					}
				}
				break;
			}
		}
	}

	for (i=0; i < 3; i++) {
		pts[i] = &nearest_interpolation_cells[thread].pixel[i]->center_pt;
		sb[i] = &nearest_interpolation_cells[thread].pixel[i]->surface_brightness;
	}

	if (nearest_interpolation_cells[thread].found_containing_cell==false) die("could not find containing cell");
	double d, total_sb=0;
	d = ((*pts[0])[0]-(*pts[1])[0])*((*pts[1])[1]-(*pts[2])[1]) - ((*pts[1])[0]-(*pts[2])[0])*((*pts[0])[1]-(*pts[1])[1]);
	total_sb += (*sb[0])*(input_center_pt[0]*((*pts[1])[1]-(*pts[2])[1]) + input_center_pt[1]*((*pts[2])[0]-(*pts[1])[0]) + (*pts[1])[0]*(*pts[2])[1] - (*pts[1])[1]*(*pts[2])[0]);
	total_sb += (*sb[1])*(input_center_pt[0]*((*pts[2])[1]-(*pts[0])[1]) + input_center_pt[1]*((*pts[0])[0]-(*pts[2])[0]) + (*pts[0])[1]*(*pts[2])[0] - (*pts[0])[0]*(*pts[2])[1]);
	total_sb += (*sb[2])*(input_center_pt[0]*((*pts[0])[1]-(*pts[1])[1]) + input_center_pt[1]*((*pts[1])[0]-(*pts[0])[0]) + (*pts[0])[0]*(*pts[1])[1] - (*pts[0])[1]*(*pts[1])[0]);
	total_sb /= d;
	return total_sb;
}

void SourcePixelGrid::find_interpolation_cells(lensvector &input_center_pt, const int& thread)
{
	int i,j,side;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if ((input_center_pt[0] >= cell[i][j]->corner_pt[0][0]) and (input_center_pt[0] < cell[i][j]->corner_pt[2][0]) and (input_center_pt[1] >= cell[i][j]->corner_pt[0][1]) and (input_center_pt[1] < cell[i][j]->corner_pt[3][1])) {
				if (cell[i][j]->cell != NULL) cell[i][j]->find_interpolation_cells(input_center_pt,thread);
				else {
					nearest_interpolation_cells[thread].found_containing_cell = true;
					nearest_interpolation_cells[thread].pixel[0] = cell[i][j];
					if (((input_center_pt[0] > cell[i][j]->center_pt[0]) and (cell[i][j]->neighbor[0] != NULL)) or (cell[i][j]->neighbor[1] == NULL)) {
						if (cell[i][j]->neighbor[0]->cell != NULL) {
							side=0;
							nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[0]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[0];
					} else {
						if (cell[i][j]->neighbor[1]->cell != NULL) {
							side=1;
							nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[1]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[1] = cell[i][j]->neighbor[1];
					}
					if (((input_center_pt[1] > cell[i][j]->center_pt[1]) and (cell[i][j]->neighbor[2] != NULL)) or (cell[i][j]->neighbor[3] == NULL)) {
						if (cell[i][j]->neighbor[2]->cell != NULL) {
							side=2;
							nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[2]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[2];
					} else {
						if (cell[i][j]->neighbor[3]->cell != NULL) {
							side=3;
							nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[3]->find_nearest_neighbor_cell(input_center_pt,side);
						}
						else nearest_interpolation_cells[thread].pixel[2] = cell[i][j]->neighbor[3];
					}
				}
				break;
			}
		}
	}
}

SourcePixelGrid* SourcePixelGrid::find_nearest_neighbor_cell(lensvector &input_center_pt, const int& side)
{
	int i,ncells;
	SourcePixelGrid **cells;
	if ((side==0) or (side==1)) ncells = w_N;
	else if ((side==2) or (side==3)) ncells = u_N;
	else die("side number cannot be larger than 3");
	cells = new SourcePixelGrid*[ncells];

	for (i=0; i < ncells; i++) {
		if (side==0) {
			if (cell[0][i]->cell != NULL) cells[i] = cell[0][i]->find_nearest_neighbor_cell(input_center_pt,side);
			else cells[i] = cell[0][i];
		} else if (side==1) {
			if (cell[u_N-1][i]->cell != NULL) cells[i] = cell[u_N-1][i]->find_nearest_neighbor_cell(input_center_pt,side);
			else cells[i] = cell[u_N-1][i];
		} else if (side==2) {
			if (cell[i][0]->cell != NULL) cells[i] = cell[i][0]->find_nearest_neighbor_cell(input_center_pt,side);
			else cells[i] = cell[i][0];
		} else if (side==3) {
			if (cell[i][w_N-1]->cell != NULL) cells[i] = cell[i][w_N-1]->find_nearest_neighbor_cell(input_center_pt,side);
			else cells[i] = cell[i][w_N-1];
		}
	}
	double sqr_distance, min_sqr_distance = 1e30;
	int i_min;
	for (i=0; i < ncells; i++) {
		sqr_distance = SQR(cells[i]->center_pt[0] - input_center_pt[0]) + SQR(cells[i]->center_pt[1] - input_center_pt[1]);
		if (sqr_distance < min_sqr_distance) {
			min_sqr_distance = sqr_distance;
			i_min = i;
		}
	}
	SourcePixelGrid *closest_cell = cells[i_min];
	delete[] cells;
	return closest_cell;
}

SourcePixelGrid* SourcePixelGrid::find_nearest_neighbor_cell(lensvector &input_center_pt, const int& side, const int tiebreaker_side)
{
	int i,ncells;
	SourcePixelGrid **cells;
	if ((side==0) or (side==1)) ncells = w_N;
	else if ((side==2) or (side==3)) ncells = u_N;
	else die("side number cannot be larger than 3");
	cells = new SourcePixelGrid*[ncells];
	double sqr_distance, min_sqr_distance = 1e30;
	SourcePixelGrid *closest_cell = NULL;
	int it=0, side_try=side;

	while ((closest_cell==NULL) and (it++ < 2))
	{
		for (i=0; i < ncells; i++) {
			if (side_try==0) {
				if (cell[0][i]->cell != NULL) cells[i] = cell[0][i]->find_nearest_neighbor_cell(input_center_pt,side);
				else cells[i] = cell[0][i];
			} else if (side_try==1) {
				if (cell[u_N-1][i]->cell != NULL) cells[i] = cell[u_N-1][i]->find_nearest_neighbor_cell(input_center_pt,side);
				else cells[i] = cell[u_N-1][i];
			} else if (side_try==2) {
				if (cell[i][0]->cell != NULL) cells[i] = cell[i][0]->find_nearest_neighbor_cell(input_center_pt,side);
				else cells[i] = cell[i][0];
			} else if (side_try==3) {
				if (cell[i][w_N-1]->cell != NULL) cells[i] = cell[i][w_N-1]->find_nearest_neighbor_cell(input_center_pt,side);
				else cells[i] = cell[i][w_N-1];
			}
		}
		for (i=0; i < ncells; i++) {
			sqr_distance = SQR(cells[i]->center_pt[0] - input_center_pt[0]) + SQR(cells[i]->center_pt[1] - input_center_pt[1]);
			if ((sqr_distance < min_sqr_distance) or ((sqr_distance==min_sqr_distance) and (i==tiebreaker_side))) {
				//if (cells[i]->active_pixel) {
					min_sqr_distance = sqr_distance;
					closest_cell = cells[i];
				//}
			}
		}
		if (closest_cell==NULL) {
			// in this case neither of the subcells in question mapped to the image plane, so we had better try again with the other two subcells.
			if (side_try==0) side_try = 1;
			else if (side_try==1) side_try = 0;
			else if (side_try==2) side_try = 3;
			else if (side_try==3) side_try = 2;
		}
	}
	delete[] cells;
	return closest_cell;
}

void SourcePixelGrid::find_nearest_two_cells(SourcePixelGrid* &cellptr1, SourcePixelGrid* &cellptr2, const int& side)
{
	if ((u_N != 2) or (w_N != 2)) die("cannot find nearest two cells unless splitting is two in either direction");
	if (side==0) {
		if (cell[0][0]->cell == NULL) cellptr1 = cell[0][0];
		else cellptr1 = cell[0][0]->find_corner_cell(0,1);
		if (cell[0][1]->cell == NULL) cellptr2 = cell[0][1];
		else cellptr2 = cell[0][1]->find_corner_cell(0,0);
	} else if (side==1) {
		if (cell[1][0]->cell == NULL) cellptr1 = cell[1][0];
		else cellptr1 = cell[1][0]->find_corner_cell(1,1);
		if (cell[1][1]->cell == NULL) cellptr2 = cell[1][1];
		else cellptr2 = cell[1][1]->find_corner_cell(1,0);
	} else if (side==2) {
		if (cell[0][0]->cell == NULL) cellptr1 = cell[0][0];
		else cellptr1 = cell[0][0]->find_corner_cell(1,0);
		if (cell[1][0]->cell == NULL) cellptr2 = cell[1][0];
		else cellptr2 = cell[1][0]->find_corner_cell(0,0);
	} else if (side==3) {
		if (cell[0][1]->cell == NULL) cellptr1 = cell[0][1];
		else cellptr1 = cell[0][1]->find_corner_cell(1,1);
		if (cell[1][1]->cell == NULL) cellptr2 = cell[1][1];
		else cellptr2 = cell[1][1]->find_corner_cell(0,1);
	}
}

SourcePixelGrid* SourcePixelGrid::find_corner_cell(const int i, const int j)
{
	SourcePixelGrid* cellptr = cell[i][j];
	while (cellptr->cell != NULL)
		cellptr = cellptr->cell[i][j];
	return cellptr;
}

void SourcePixelGrid::generate_gmatrices()
{
	int i,j,k,l;
	SourcePixelGrid *cellptr1, *cellptr2;
	double alpha, beta;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->generate_gmatrices();
			else {
				if (cell[i][j]->active_pixel) {
					for (k=0; k < 4; k++) {
						lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(1);
						lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cell[i][j]->active_index);
						lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
						lens->gmatrix_nn[k]++;
						if (cell[i][j]->neighbor[k]) {
							if (cell[i][j]->neighbor[k]->cell != NULL) {
								cell[i][j]->neighbor[k]->find_nearest_two_cells(cellptr1,cellptr2,k);
								//cout << "cell 1: " << cellptr1->center_pt[0] << " " << cellptr1->center_pt[1] << endl;
								//cout << "cell 2: " << cellptr2->center_pt[0] << " " << cellptr2->center_pt[1] << endl;
								if ((cellptr1==NULL) or (cellptr2==NULL)) die("Hmm, not getting back two cells");
								if (k < 2) {
									// interpolating surface brightness along x-direction
									alpha = abs((cell[i][j]->center_pt[1] - cellptr1->center_pt[1]) / (cellptr2->center_pt[1] - cellptr1->center_pt[1]));
								} else {
									// interpolating surface brightness along y-direction
									alpha = abs((cell[i][j]->center_pt[0] - cellptr1->center_pt[0]) / (cellptr2->center_pt[0] - cellptr1->center_pt[0]));
								}
								beta = 1-alpha;
								if (cellptr1->active_pixel) {
									if (!cellptr2->active_pixel) beta=1; // just in case the other point is no good
									lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-beta);
									lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr1->active_index);
									lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
									lens->gmatrix_nn[k]++;
								}
								if (cellptr2->active_pixel) {
									if (!cellptr1->active_pixel) alpha=1; // just in case the other point is no good
									lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-alpha);
									lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr2->active_index);
									lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
									lens->gmatrix_nn[k]++;
								}
							}
							else if (cell[i][j]->neighbor[k]->active_pixel) {
								if (cell[i][j]->neighbor[k]->level==cell[i][j]->level) {
									lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-1);
									lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cell[i][j]->neighbor[k]->active_index);
									lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
									lens->gmatrix_nn[k]++;
								} else {
									cellptr1 = cell[i][j]->neighbor[k];
									if (k < 2) {
										if (cellptr1->center_pt[1] > cell[i][j]->center_pt[1]) l=3;
										else l=2;
									} else {
										if (cellptr1->center_pt[0] > cell[i][j]->center_pt[0]) l=1;
										else l=0;
									}
									if ((cellptr1->neighbor[l]==NULL) or ((cellptr1->neighbor[l]->cell==NULL) and (!cellptr1->neighbor[l]->active_pixel))) {
										// There is no useful nearby neighbor to interpolate with, so just use the single neighbor pixel
										lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-1);
										lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr1->active_index);
										lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
										lens->gmatrix_nn[k]++;
									} else {
										if (cellptr1->neighbor[l]->cell==NULL) cellptr2 = cellptr1->neighbor[l];
										else cellptr2 = cellptr1->neighbor[l]->find_nearest_neighbor_cell(cellptr1->center_pt,l,k%2); // the tiebreaker k%2 ensures that preference goes to cells that are closer to this cell in order to interpolate to find the gradient
										if (cellptr2==NULL) die("Subcell does not map to source pixel; regularization currently cannot handle unmapped subcells");
										if (k < 2) alpha = abs((cell[i][j]->center_pt[1] - cellptr1->center_pt[1]) / (cellptr2->center_pt[1] - cellptr1->center_pt[1]));
										else alpha = abs((cell[i][j]->center_pt[0] - cellptr1->center_pt[0]) / (cellptr2->center_pt[0] - cellptr1->center_pt[0]));
										beta = 1-alpha;
										//cout << alpha << " " << beta << " " << k << " " << l << " " << ii << " " << jj << " " << i << " " << j << endl;
										//cout << cell[i][j]->center_pt[0] << " " << cellptr1->center_pt[0] << " " << cellptr1->center_pt[1] << " " << cellptr2->center_pt[0] << " " << cellptr2->center_pt[1] << endl;
										if (cellptr1->active_pixel) {
											if (!cellptr2->active_pixel) beta=1; // just in case the other point is no good
											lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-beta);
											lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr1->active_index);
											lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
											lens->gmatrix_nn[k]++;
										}
										if (cellptr2->active_pixel) {
											if (!cellptr1->active_pixel) alpha=1; // just in case the other point is no good
											lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-alpha);
											lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr2->active_index);
											lens->gmatrix_row_nn[k][cell[i][j]->active_index]++;
											lens->gmatrix_nn[k]++;
										}

										//lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-beta);
										//lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr1->active_index);
										//lens->gmatrix_rows[k][cell[i][j]->active_index].push_back(-alpha);
										//lens->gmatrix_index_rows[k][cell[i][j]->active_index].push_back(cellptr2->active_index);
										//lens->gmatrix_row_nn[k][cell[i][j]->active_index] += 2;
										//lens->gmatrix_nn[k] += 2;
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void SourcePixelGrid::generate_hmatrices()
{
	int i,j,k,l,m,kmin,kmax;
	SourcePixelGrid *cellptr1, *cellptr2;
	double alpha, beta;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->generate_hmatrices();
			else {
				for (l=0; l < 2; l++) {
					if (cell[i][j]->active_pixel) {
						lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(-2);
						lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cell[i][j]->active_index);
						lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
						lens->hmatrix_nn[l]++;
						if (l==0) {
							kmin=0; kmax=1;
						} else {
							kmin=2; kmax=3;
						}
						for (k=kmin; k <= kmax; k++) {
							if (cell[i][j]->neighbor[k]) {
								if (cell[i][j]->neighbor[k]->cell != NULL) {
									cell[i][j]->neighbor[k]->find_nearest_two_cells(cellptr1,cellptr2,k);
									if ((cellptr1==NULL) or (cellptr2==NULL)) die("Hmm, not getting back two cells");
									if (k < 2) {
										// interpolating surface brightness along x-direction
										alpha = abs((cell[i][j]->center_pt[1] - cellptr1->center_pt[1]) / (cellptr2->center_pt[1] - cellptr1->center_pt[1]));
									} else {
										// interpolating surface brightness along y-direction
										alpha = abs((cell[i][j]->center_pt[0] - cellptr1->center_pt[0]) / (cellptr2->center_pt[0] - cellptr1->center_pt[0]));
									}
									beta = 1-alpha;
									if (!cellptr1->active_pixel) alpha=1;
									if (!cellptr2->active_pixel) beta=1;
									if (cellptr1->active_pixel) {
										lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(beta);
										lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr1->active_index);
										lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
										lens->hmatrix_nn[l]++;
									}
									if (cellptr2->active_pixel) {
										lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(alpha);
										lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr2->active_index);
										lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
										lens->hmatrix_nn[l]++;
									}

								}
								else if (cell[i][j]->neighbor[k]->active_pixel) {
									if (cell[i][j]->neighbor[k]->level==cell[i][j]->level) {
										lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(1);
										lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cell[i][j]->neighbor[k]->active_index);
										lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
										lens->hmatrix_nn[l]++;
									} else {
										cellptr1 = cell[i][j]->neighbor[k];
										if (k < 2) {
											if (cellptr1->center_pt[1] > cell[i][j]->center_pt[1]) m=3;
											else m=2;
										} else {
											if (cellptr1->center_pt[0] > cell[i][j]->center_pt[0]) m=1;
											else m=0;
										}
										if ((cellptr1->neighbor[m]==NULL) or ((cellptr1->neighbor[m]->cell==NULL) and (!cellptr1->neighbor[m]->active_pixel))) {
											// There is no useful nearby neighbor to interpolate with, so just use the single neighbor pixel
											lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(1);
											lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr1->active_index);
											lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
											lens->hmatrix_nn[l]++;
										} else {
											if (cellptr1->neighbor[m]->cell==NULL) cellptr2 = cellptr1->neighbor[m];
											else cellptr2 = cellptr1->neighbor[m]->find_nearest_neighbor_cell(cellptr1->center_pt,m,k%2); // the tiebreaker k%2 ensures that preference goes to cells that are closer to this cell in order to interpolate to find the curvature
											if (cellptr2==NULL) die("Subcell does not map to source pixel; regularization currently cannot handle unmapped subcells");
											if (k < 2) alpha = abs((cell[i][j]->center_pt[1] - cellptr1->center_pt[1]) / (cellptr2->center_pt[1] - cellptr1->center_pt[1]));
											else alpha = abs((cell[i][j]->center_pt[0] - cellptr1->center_pt[0]) / (cellptr2->center_pt[0] - cellptr1->center_pt[0]));
											beta = 1-alpha;
											//cout << alpha << " " << beta << " " << k << " " << m << " " << ii << " " << jj << " " << i << " " << j << endl;
											//cout << cell[i][j]->center_pt[0] << " " << cellptr1->center_pt[0] << " " << cellptr1->center_pt[1] << " " << cellptr2->center_pt[0] << " " << cellptr2->center_pt[1] << endl;
											if (!cellptr1->active_pixel) alpha=1;
											if (!cellptr2->active_pixel) beta=1;
											if (cellptr1->active_pixel) {
												lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(beta);
												lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr1->active_index);
												lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
												lens->hmatrix_nn[l]++;
											}
											if (cellptr2->active_pixel) {
												lens->hmatrix_rows[l][cell[i][j]->active_index].push_back(alpha);
												lens->hmatrix_index_rows[l][cell[i][j]->active_index].push_back(cellptr2->active_index);
												lens->hmatrix_row_nn[l][cell[i][j]->active_index]++;
												lens->hmatrix_nn[l]++;
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void Lens::generate_Rmatrix_from_hmatrices()
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int i,j,k,l,m,n,indx;

	vector<int> *jvals[2];
	vector<int> *lvals[2];
	for (i=0; i < 2; i++) {
		jvals[i] = new vector<int>[source_npixels];
		lvals[i] = new vector<int>[source_npixels];
	}

	Rmatrix_diags = new double[source_npixels];
	Rmatrix_rows = new vector<double>[source_npixels];
	Rmatrix_index_rows = new vector<int>[source_npixels];
	Rmatrix_row_nn = new int[source_npixels];
	Rmatrix_nn = 0;
	int Rmatrix_nn_part = 0;
	for (j=0; j < source_npixels; j++) {
		Rmatrix_diags[j] = 0;
		Rmatrix_row_nn[j] = 0;
	}

	bool new_entry;
	int src_index1, src_index2, col_index, col_i;
	double tmp, element;
	int tmp_i;

	for (k=0; k < 2; k++) {
		hmatrix_rows[k] = new vector<double>[source_npixels];
		hmatrix_index_rows[k] = new vector<int>[source_npixels];
		hmatrix_row_nn[k] = new int[source_npixels];
		hmatrix_nn[k] = 0;
		for (j=0; j < source_npixels; j++) {
			hmatrix_row_nn[k][j] = 0;
		}
	}
	source_pixel_grid->generate_hmatrices();

	for (k=0; k < 2; k++) {
		hmatrix[k] = new double[hmatrix_nn[k]];
		hmatrix_index[k] = new int[hmatrix_nn[k]];
		hmatrix_row_index[k] = new int[source_npixels+1];

		hmatrix_row_index[k][0] = 0;
		for (i=0; i < source_npixels; i++)
			hmatrix_row_index[k][i+1] = hmatrix_row_index[k][i] + hmatrix_row_nn[k][i];
		if (hmatrix_row_index[k][source_npixels] != hmatrix_nn[k]) die("the number of elements don't match up for hmatrix %i",k);

		for (i=0; i < source_npixels; i++) {
			indx = hmatrix_row_index[k][i];
			for (j=0; j < hmatrix_row_nn[k][i]; j++) {
				hmatrix[k][indx+j] = hmatrix_rows[k][i][j];
				hmatrix_index[k][indx+j] = hmatrix_index_rows[k][i][j];
			}
		}
		delete[] hmatrix_rows[k];
		delete[] hmatrix_index_rows[k];
		delete[] hmatrix_row_nn[k];

		for (i=0; i < source_npixels; i++) {
			for (j=hmatrix_row_index[k][i]; j < hmatrix_row_index[k][i+1]; j++) {
				for (l=j; l < hmatrix_row_index[k][i+1]; l++) {
					src_index1 = hmatrix_index[k][j];
					src_index2 = hmatrix_index[k][l];
					if (src_index1 > src_index2) {
						tmp=src_index1;
						src_index1=src_index2;
						src_index2=tmp;
						jvals[k][src_index1].push_back(l);
						lvals[k][src_index1].push_back(j);
					} else {
						jvals[k][src_index1].push_back(j);
						lvals[k][src_index1].push_back(l);
					}
				}
			}
		}
	}

	#pragma omp parallel for private(i,j,k,l,m,n,src_index1,src_index2,new_entry,col_index,col_i,element) schedule(static) reduction(+:Rmatrix_nn_part)
	for (src_index1=0; src_index1 < source_npixels; src_index1++) {
		for (k=0; k < 2; k++) {
			col_i=0;
			for (n=0; n < jvals[k][src_index1].size(); n++) {
				j = jvals[k][src_index1][n];
				l = lvals[k][src_index1][n];
				src_index2 = hmatrix_index[k][l];
				new_entry = true;
				element = hmatrix[k][j]*hmatrix[k][l]; // generalize this to full covariance matrix later
				if (src_index1==src_index2) Rmatrix_diags[src_index1] += element;
				else {
					m=0;
					while ((m < Rmatrix_row_nn[src_index1]) and (new_entry==true)) {
						if (Rmatrix_index_rows[src_index1][m]==src_index2) {
							new_entry = false;
							col_index = m;
						}
						m++;
					}
					if (new_entry) {
						Rmatrix_rows[src_index1].push_back(element);
						Rmatrix_index_rows[src_index1].push_back(src_index2);
						Rmatrix_row_nn[src_index1]++;
						col_i++;
					}
					else Rmatrix_rows[src_index1][col_index] += element;
				}
			}
			Rmatrix_nn_part += col_i;
		}
	}

	for (k=0; k < 2; k++) {
		delete[] hmatrix[k];
		delete[] hmatrix_index[k];
		delete[] hmatrix_row_index[k];
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating Rmatrix: " << wtime << endl;
	}
#endif

	Rmatrix_nn = Rmatrix_nn_part;
	Rmatrix_nn += source_npixels+1;

	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];

	for (i=0; i < source_npixels; i++)
		Rmatrix[i] = Rmatrix_diags[i];

	Rmatrix_index[0] = source_npixels+1;
	for (i=0; i < source_npixels; i++) {
		Rmatrix_index[i+1] = Rmatrix_index[i] + Rmatrix_row_nn[i];
	}

	for (i=0; i < source_npixels; i++) {
		indx = Rmatrix_index[i];
		for (j=0; j < Rmatrix_row_nn[i]; j++) {
			Rmatrix[indx+j] = Rmatrix_rows[i][j];
			Rmatrix_index[indx+j] = Rmatrix_index_rows[i][j];
		}
	}

	delete[] Rmatrix_row_nn;
	delete[] Rmatrix_diags;
	delete[] Rmatrix_rows;
	delete[] Rmatrix_index_rows;

	for (i=0; i < 2; i++) {
		delete[] jvals[i];
		delete[] lvals[i];
	}
}

void Lens::generate_Rmatrix_from_gmatrices()
{
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int i,j,k,l,m,n,indx;

	vector<int> *jvals[4];
	vector<int> *lvals[4];
	for (i=0; i < 4; i++) {
		jvals[i] = new vector<int>[source_npixels];
		lvals[i] = new vector<int>[source_npixels];
	}

	Rmatrix_diags = new double[source_npixels];
	Rmatrix_rows = new vector<double>[source_npixels];
	Rmatrix_index_rows = new vector<int>[source_npixels];
	Rmatrix_row_nn = new int[source_npixels];
	Rmatrix_nn = 0;
	int Rmatrix_nn_part = 0;
	for (j=0; j < source_npixels; j++) {
		Rmatrix_diags[j] = 0;
		Rmatrix_row_nn[j] = 0;
	}

	bool new_entry;
	int src_index1, src_index2, col_index, col_i;
	double tmp, element;
	int tmp_i;

	for (k=0; k < 4; k++) {
		gmatrix_rows[k] = new vector<double>[source_npixels];
		gmatrix_index_rows[k] = new vector<int>[source_npixels];
		gmatrix_row_nn[k] = new int[source_npixels];
		gmatrix_nn[k] = 0;
		for (j=0; j < source_npixels; j++) {
			gmatrix_row_nn[k][j] = 0;
		}
	}
	source_pixel_grid->generate_gmatrices();

	for (k=0; k < 4; k++) {
		gmatrix[k] = new double[gmatrix_nn[k]];
		gmatrix_index[k] = new int[gmatrix_nn[k]];
		gmatrix_row_index[k] = new int[source_npixels+1];

		gmatrix_row_index[k][0] = 0;
		for (i=0; i < source_npixels; i++)
			gmatrix_row_index[k][i+1] = gmatrix_row_index[k][i] + gmatrix_row_nn[k][i];
		if (gmatrix_row_index[k][source_npixels] != gmatrix_nn[k]) die("the number of elements don't match up for gmatrix %i",k);

		for (i=0; i < source_npixels; i++) {
			indx = gmatrix_row_index[k][i];
			for (j=0; j < gmatrix_row_nn[k][i]; j++) {
				gmatrix[k][indx+j] = gmatrix_rows[k][i][j];
				gmatrix_index[k][indx+j] = gmatrix_index_rows[k][i][j];
			}
		}
		delete[] gmatrix_rows[k];
		delete[] gmatrix_index_rows[k];
		delete[] gmatrix_row_nn[k];

		for (i=0; i < source_npixels; i++) {
			for (j=gmatrix_row_index[k][i]; j < gmatrix_row_index[k][i+1]; j++) {
				for (l=j; l < gmatrix_row_index[k][i+1]; l++) {
					src_index1 = gmatrix_index[k][j];
					src_index2 = gmatrix_index[k][l];
					if (src_index1 > src_index2) {
						tmp=src_index1;
						src_index1=src_index2;
						src_index2=tmp;
						jvals[k][src_index1].push_back(l);
						lvals[k][src_index1].push_back(j);
					} else {
						jvals[k][src_index1].push_back(j);
						lvals[k][src_index1].push_back(l);
					}
				}
			}
		}
	}

	#pragma omp parallel for private(i,j,k,l,m,n,src_index1,src_index2,new_entry,col_index,col_i,element) schedule(static) reduction(+:Rmatrix_nn_part)
	for (src_index1=0; src_index1 < source_npixels; src_index1++) {
		for (k=0; k < 4; k++) {
			col_i=0;
			for (n=0; n < jvals[k][src_index1].size(); n++) {
				j = jvals[k][src_index1][n];
				l = lvals[k][src_index1][n];
				src_index2 = gmatrix_index[k][l];
				new_entry = true;
				element = gmatrix[k][j]*gmatrix[k][l]; // generalize this to full covariance matrix later
				if (src_index1==src_index2) Rmatrix_diags[src_index1] += element;
				else {
					m=0;
					while ((m < Rmatrix_row_nn[src_index1]) and (new_entry==true)) {
						if (Rmatrix_index_rows[src_index1][m]==src_index2) {
							new_entry = false;
							col_index = m;
						}
						m++;
					}
					if (new_entry) {
						Rmatrix_rows[src_index1].push_back(element);
						Rmatrix_index_rows[src_index1].push_back(src_index2);
						Rmatrix_row_nn[src_index1]++;
						col_i++;
					}
					else Rmatrix_rows[src_index1][col_index] += element;
				}
			}
			Rmatrix_nn_part += col_i;
		}
	}

	for (k=0; k < 4; k++) {
		delete[] gmatrix[k];
		delete[] gmatrix_index[k];
		delete[] gmatrix_row_index[k];
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating Rmatrix: " << wtime << endl;
	}
#endif

	Rmatrix_nn = Rmatrix_nn_part;
	Rmatrix_nn += source_npixels+1;

	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];

	for (i=0; i < source_npixels; i++)
		Rmatrix[i] = Rmatrix_diags[i];

	Rmatrix_index[0] = source_npixels+1;
	for (i=0; i < source_npixels; i++) {
		Rmatrix_index[i+1] = Rmatrix_index[i] + Rmatrix_row_nn[i];
	}

	for (i=0; i < source_npixels; i++) {
		indx = Rmatrix_index[i];
		for (j=0; j < Rmatrix_row_nn[i]; j++) {
			Rmatrix[indx+j] = Rmatrix_rows[i][j];
			Rmatrix_index[indx+j] = Rmatrix_index_rows[i][j];
		}
	}

	delete[] Rmatrix_row_nn;
	delete[] Rmatrix_diags;
	delete[] Rmatrix_rows;
	delete[] Rmatrix_index_rows;

	for (i=0; i < 4; i++) {
		delete[] jvals[i];
		delete[] lvals[i];
	}
}

int SourcePixelGrid::assign_indices_and_count_levels()
{
	levels=1; // we are going to recount the number of levels
	int source_pixel_i=0;
	assign_indices(source_pixel_i);
	return source_pixel_i;
}

void SourcePixelGrid::assign_indices(int& source_pixel_i)
{
	if (levels < level+1) levels=level+1;
	int i, j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->assign_indices(source_pixel_i);
			else {
				cell[i][j]->index = source_pixel_i++;
			}
		}
	}
}

ofstream SourcePixelGrid::index_out;

void SourcePixelGrid::print_indices()
{
	int i, j;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->print_indices();
			else {
				index_out << cell[i][j]->index << " " << cell[i][j]->active_index << " level=" << cell[i][j]->level << endl;
			}
		}
	}
}

bool SourcePixelGrid::regrid_if_unmapped_source_subcells;
bool SourcePixelGrid::activate_unmapped_source_pixels;
bool SourcePixelGrid::exclude_source_pixels_outside_fit_window;

int SourcePixelGrid::assign_active_indices_and_count_source_pixels(bool regrid_if_inactive_cells, bool activate_unmapped_pixels, bool exclude_pixels_outside_window)
{
	regrid_if_unmapped_source_subcells = regrid_if_inactive_cells;
	activate_unmapped_source_pixels = activate_unmapped_pixels;
	exclude_source_pixels_outside_fit_window = exclude_pixels_outside_window;
	int source_pixel_i=0;
	assign_active_indices(source_pixel_i);
	return source_pixel_i;
}

ofstream SourcePixelGrid::missed_cells_out; // remove later?

void SourcePixelGrid::assign_active_indices(int& source_pixel_i)
{
	int i, j;
	bool unsplit_cell = false;
	for (j=0; j < w_N; j++) {
		for (i=0; i < u_N; i++) {
			if (cell[i][j]->cell != NULL) cell[i][j]->assign_active_indices(source_pixel_i);
			else {
					//cell[i][j]->active_index = source_pixel_i++;
					//cell[i][j]->active_pixel = true;
				if (cell[i][j]->maps_to_image_pixel) {
					cell[i][j]->active_index = source_pixel_i++;
					cell[i][j]->active_pixel = true;
				} else {
					if (lens->mpi_id==0) warn(lens->warnings,"A source pixel does not map to any image pixel (for source pixel %i,%i), level %i, center (%g,%g)",i,j,cell[i][j]->level,cell[i][j]->center_pt[0],cell[i][j]->center_pt[1]);
					if ((activate_unmapped_source_pixels) and ((!regrid_if_unmapped_source_subcells) or (level==0))) { // if we are removing unmapped subpixels, we may still want to activate first-level unmapped pixels
						if ((exclude_source_pixels_outside_fit_window) and (cell[i][j]->maps_to_image_window==false)) ;
						else {
							cell[i][j]->active_index = source_pixel_i++;
							cell[i][j]->active_pixel = true;
						}
					} else {
						cell[i][j]->active_pixel = false;
						if ((regrid_if_unmapped_source_subcells) and (level >= 1)) {
							if (!regrid) regrid = true;
							unsplit_cell = true;
						}
						//missed_cells_out << cell[i][j]->center_pt[0] << " " << cell[i][j]->center_pt[1] << endl;
					}
					//if ((exclude_source_pixels_outside_fit_window) and (cell[i][j]->maps_to_image_window==false)) {
						//if (cell[i][j]->active_pixel==true) {
							//source_pixel_i--;
							//if (!regrid) regrid = true;
							//cell[i][j]->active_pixel = false;
						//}
					//}
				}
			}
		}
	}
	if (unsplit_cell) unsplit();
}

SourcePixelGrid::~SourcePixelGrid()
{
	if (cell != NULL) {
		int i,j;
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) delete cell[i][j];
			delete[] cell[i];
		}
		cell = NULL;
	}
}

void SourcePixelGrid::clear()
{
	if (cell == NULL) return;

	int i,j;
	for (i=0; i < u_N; i++) {
		for (j=0; j < w_N; j++) delete cell[i][j];
		delete[] cell[i];
	}
	delete[] cell;
	cell = NULL;
	u_N=1; w_N=1;
}

void SourcePixelGrid::clear_subgrids()
{
	if (level>0) {
		if (cell == NULL) return;
		int i,j;
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				delete cell[i][j];
			}
			delete[] cell[i];
		}
		delete[] cell;
		cell = NULL;
		number_of_pixels -= (u_N*w_N - 1);
		u_N=1; w_N=1;
	} else {
		int i,j;
		for (i=0; i < u_N; i++) {
			for (j=0; j < w_N; j++) {
				if (cell[i][j]->cell != NULL) cell[i][j]->clear_subgrids();
			}
		}
	}
}

/***************************************** Functions in class ImagePixelGrid ****************************************/

void ImagePixelData::load_data(string root)
{
	string sbfilename = root + ".dat";
	string xfilename = root + ".x";
	string yfilename = root + ".y";

	int i,j;
	double dummy;
	if (xvals != NULL) delete[] xvals;
	if (yvals != NULL) delete[] yvals;
	if (surface_brightness != NULL) {
		for (i=0; i < npixels_x; i++) delete[] surface_brightness[i];
		delete[] surface_brightness;
	}
	if (require_fit != NULL) {
		for (i=0; i < npixels_x; i++) delete[] require_fit[i];
		delete[] require_fit;
	}

	ifstream xfile(xfilename.c_str());
	i=0;
	while (xfile >> dummy) i++;
	xfile.close();
	npixels_x = i-1;

	ifstream yfile(yfilename.c_str());
	j=0;
	while (yfile >> dummy) j++;
	yfile.close();
	npixels_y = j-1;

	n_required_pixels = npixels_x*npixels_y;
	xvals = new double[npixels_x+1];
	xfile.open(xfilename.c_str());
	for (i=0; i <= npixels_x; i++) xfile >> xvals[i];
	yvals = new double[npixels_y+1];
	yfile.open(yfilename.c_str());
	for (i=0; i <= npixels_y; i++) yfile >> yvals[i];

	ifstream sbfile(sbfilename.c_str());
	surface_brightness = new double*[npixels_x];
	require_fit = new bool*[npixels_x];
	for (i=0; i < npixels_x; i++) {
		surface_brightness[i] = new double[npixels_y];
		require_fit[i] = new bool[npixels_y];
		for (j=0; j < npixels_y; j++) require_fit[i][j] = true;
	}
	for (j=0; j < npixels_y; j++) {
		for (i=0; i < npixels_x; i++) sbfile >> surface_brightness[i][j];
	}
}

bool ImagePixelData::load_data_fits(bool use_pixel_size, string fits_filename)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to read FITS files\n"; return false;
#else
	bool image_load_status = false;
	int i,j,kk;
	if (xvals != NULL) delete[] xvals;
	if (yvals != NULL) delete[] yvals;
	if (surface_brightness != NULL) {
		for (i=0; i < npixels_x; i++) delete[] surface_brightness[i];
		delete[] surface_brightness;
	}
	if (require_fit != NULL) {
		for (i=0; i < npixels_x; i++) delete[] require_fit[i];
		delete[] require_fit;
	}

	fitsfile *fptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix, naxis;
	long naxes[2] = {1,1};
	double *pixels;
	double x, y, xstep, ystep;

	if (!fits_open_file(&fptr, fits_filename.c_str(), READONLY, &status))
	{
		if (!fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status) )
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				npixels_x = naxes[0];
				npixels_y = naxes[1];
				n_required_pixels = npixels_x*npixels_y;
				xvals = new double[npixels_x+1];
				yvals = new double[npixels_y+1];
				if (use_pixel_size) {
					xstep = ystep = pixel_size;
					xmax = 0.5*npixels_x*pixel_size;
					ymax = 0.5*npixels_y*pixel_size;
					xmin=-xmax; ymin=-ymax;
				} else {
					xstep = (xmax-xmin)/npixels_x;
					ystep = (ymax-ymin)/npixels_y;
				}
				for (i=0, x=xmin; i <= npixels_x; i++, x += xstep) xvals[i] = x;
				for (i=0, y=ymin; i <= npixels_y; i++, y += ystep) yvals[i] = y;
				pixels = new double[npixels_x];
				surface_brightness = new double*[npixels_x];
				require_fit = new bool*[npixels_x];
				for (i=0; i < npixels_x; i++) {
					surface_brightness[i] = new double[npixels_y];
					require_fit[i] = new bool[npixels_y];
					for (j=0; j < npixels_y; j++) require_fit[i][j] = true;
				}

				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					if (fits_read_pix(fptr, TDOUBLE, fpixel, naxes[0], NULL, pixels, NULL, &status) )  // read row of pixels
						break; // jump out of loop on error

					for (i=0; i < naxes[0]; i++) {
						surface_brightness[i][j] = pixels[i];
					}
				}
				delete[] pixels;
				image_load_status = true;
			}
		}
		fits_close_file(fptr, &status);
	} 

	if (status) fits_report_error(stderr, status); // print any error message
	return image_load_status;
#endif
}

bool ImagePixelData::load_mask_fits(string fits_filename)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to read FITS files\n"; return false;
#else
	bool image_load_status = false;
	int i,j,kk;

	fitsfile *fptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix, naxis;
	long naxes[2] = {1,1};
	double *pixels;
	double x, y, xstep, ystep;

	if (!fits_open_file(&fptr, fits_filename.c_str(), READONLY, &status))
	{
		if (!fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status) )
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				if ((naxes[0] != npixels_x) or (naxes[1] != npixels_y)) { cout << "Error: number of pixels in mask file does not match number of pixels in loaded data\n"; return false; }
				pixels = new double[npixels_x];
				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					if (fits_read_pix(fptr, TDOUBLE, fpixel, naxes[0], NULL, pixels, NULL, &status) )  // read row of pixels
						break; // jump out of loop on error

					for (i=0; i < naxes[0]; i++) {
						if (pixels[i] == 0.0) require_fit[i][j] = false;
						else require_fit[i][j] = true;
					}
				}
				delete[] pixels;
				image_load_status = true;
			}
		}
		fits_close_file(fptr, &status);
	} 

	if (status) fits_report_error(stderr, status); // print any error message
	return image_load_status;
#endif
}

bool Lens::load_psf_fits(string fits_filename)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to read FITS files\n"; return false;
#else
	use_input_psf_matrix = true;
	bool image_load_status = false;
	int i,j,kk;
	if (psf_matrix != NULL) {
		for (i=0; i < psf_npixels_x; i++) delete[] psf_matrix[i];
		delete[] psf_matrix;
		psf_matrix = NULL;
	}
	double **input_psf_matrix;

	fitsfile *fptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix, naxis;
	int nx, ny;
	long naxes[2] = {1,1};
	double *pixels;
	double x, y, xstep, ystep;

	if (!fits_open_file(&fptr, fits_filename.c_str(), READONLY, &status))
	{
		if (!fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status) )
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				nx = naxes[0];
				ny = naxes[1];
				pixels = new double[nx];
				input_psf_matrix = new double*[nx];
				for (i=0; i < nx; i++) input_psf_matrix[i] = new double[ny];
				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					if (fits_read_pix(fptr, TDOUBLE, fpixel, naxes[0], NULL, pixels, NULL, &status) )  // read row of pixels
						break; // jump out of loop on error

					for (i=0; i < naxes[0]; i++) {
						input_psf_matrix[i][j] = pixels[i];
					}
				}
				delete[] pixels;
				image_load_status = true;
			}
		}
		fits_close_file(fptr, &status);
	} 
	int imid, jmid, imin, imax, jmin, jmax;
	imid = nx/2;
	jmid = ny/2;
	imin = imid;
	imax = imid;
	jmin = jmid;
	jmax = jmid;
	for (i=0; i < nx; i++) {
		for (j=0; j < ny; j++) {
			if (input_psf_matrix[i][j] > psf_threshold) {
				if (i < imin) imin=i;
				if (i > imax) imax=i;
				if (j < jmin) jmin=j;
				if (j > jmax) jmax=j;
			}
		}
	}
	int nx_half, ny_half;
	nx_half = (imax-imin+1)/2;
	ny_half = (jmax-jmin+1)/2;
	psf_npixels_x = 2*nx_half+1;
	psf_npixels_y = 2*ny_half+1;
	psf_matrix = new double*[psf_npixels_x];
	for (i=0; i < psf_npixels_x; i++) psf_matrix[i] = new double[psf_npixels_y];
	int ii,jj;
	for (ii=0, i=imid-nx_half; ii < psf_npixels_x; i++, ii++) {
		for (jj=0, j=jmid-ny_half; jj < psf_npixels_y; j++, jj++) {
			psf_matrix[ii][jj] = input_psf_matrix[i][j];
		}
	}
	double normalization = 0;
	for (i=0; i < psf_npixels_x; i++) {
		for (j=0; j < psf_npixels_y; j++) {
			normalization += psf_matrix[i][j];
		}
	}
	for (i=0; i < psf_npixels_x; i++) {
		for (j=0; j < psf_npixels_y; j++) {
			psf_matrix[i][j] /= normalization;
		}
	}
	//for (i=0; i < psf_npixels_x; i++) {
		//for (j=0; j < psf_npixels_y; j++) {
			//cout << psf_matrix[i][j] << " ";
		//}
		//cout << endl;
	//}
	//cout << psf_npixels_x << " " << psf_npixels_y << " " << nx_half << " " << ny_half << endl;

	for (i=0; i < nx; i++) delete[] input_psf_matrix[i];
	delete[] input_psf_matrix;

	if (status) fits_report_error(stderr, status); // print any error message
	return image_load_status;
#endif
}

ImagePixelData::~ImagePixelData()
{
	if (xvals != NULL) delete[] xvals;
	if (yvals != NULL) delete[] yvals;
	if (surface_brightness != NULL) {
		for (int i=0; i < npixels_x; i++) delete[] surface_brightness[i];
		delete[] surface_brightness;
	}
}

void ImagePixelData::set_no_required_data_pixels()
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			require_fit[i][j] = false;
		}
	}
	n_required_pixels = 0;
}

void ImagePixelData::set_all_required_data_pixels()
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			require_fit[i][j] = true;
		}
	}
	n_required_pixels = npixels_x*npixels_y;
}

void ImagePixelData::set_required_data_pixels(const double xmin, const double xmax, const double ymin, const double ymax, const bool unset)
{
	int i,j;
	for (i=0; i < npixels_x; i++) {
		for (j=0; j < npixels_y; j++) {
			if ((xvals[i] > xmin) and (xvals[i+1] < xmax) and (yvals[j] > ymin) and (yvals[j+1] < ymax)) {
				if (!unset) {
					if (require_fit[i][j] == false) {
						require_fit[i][j] = true;
						n_required_pixels++;
					}
				} else {
					if (require_fit[i][j] == true) {
						require_fit[i][j] = false;
						n_required_pixels--;
					}
				}
			}
		}
	}
}

void ImagePixelData::set_required_data_annulus(const double xc, const double yc, const double rmin, const double rmax, double theta1_deg, double theta2_deg, const bool unset)
{
	// the angles MUST be between 0 and 360 here, so we enforce this in the following
	while (theta1_deg < 0) theta1_deg += 360;
	while (theta1_deg > 360) theta1_deg -= 360;
	while (theta2_deg < 0) theta2_deg += 360;
	while (theta2_deg > 360) theta2_deg -= 360;
	double x, y, rsq, rminsq, rmaxsq, theta, theta1, theta2;
	rminsq = rmin*rmin;
	rmaxsq = rmax*rmax;
	theta1 = degrees_to_radians(theta1_deg);
	theta2 = degrees_to_radians(theta2_deg);
	int i,j;
	double theta_old;
	for (i=0; i < npixels_x; i++) {
		x = 0.5*(xvals[i] + xvals[i+1]);
		for (j=0; j < npixels_y; j++) {
			y = 0.5*(yvals[j] + yvals[j+1]);
			rsq = SQR(x-xc) + SQR(y-yc);
			theta = atan(abs((y-yc)/(x-xc)));
			theta_old=theta;
			if (x < xc) {
				if (y < yc)
					theta = theta + M_PI;
				else
					theta = M_PI - theta;
			} else if (y < yc) {
				theta = M_2PI - theta;
			}
			if ((rsq > rminsq) and (rsq < rmaxsq)) {
				// allow for two possibilities: theta1 < theta2, and theta2 < theta1 (which can happen if, e.g. theta1 is input as negative and theta1 is input as positive)
				if (((theta2 > theta1) and (theta >= theta1) and (theta <= theta2)) or ((theta1 > theta2) and ((theta >= theta1) or (theta <= theta2)))) {
					if (!unset) {
						if (require_fit[i][j] == false) {
							require_fit[i][j] = true;
							n_required_pixels++;
						}
					} else {
						if (require_fit[i][j] == true) {
							require_fit[i][j] = false;
							n_required_pixels--;
						}
					}
				}
			}
		}
	}
}

void ImagePixelData::add_point_image_from_centroid(ImageData* point_image_data, const double xmin, const double xmax, const double ymin, const double ymax, const double sb_threshold, const double pixel_error)
{
	int i,j;
	int imin=0, imax=npixels_x, jmin=0, jmax=npixels_y;
	bool passed_min=false;
	for (i=0; i < npixels_x; i++) {
		if ((passed_min==false) and ((xvals[i+1]+xvals[i]) > 2*xmin)) {
			imin = i;
			passed_min = true;
		} else if (passed_min==true) {
			if ((xvals[i+1]+xvals[i]) > 2*xmax) {
				imax = i-1;
				break;
			}
		}
	}
	passed_min = false;
	for (j=0; j < npixels_y; j++) {
		if ((passed_min==false) and ((yvals[j+1]+yvals[j]) > 2*ymin)) {
			jmin = j;
			passed_min = true;
		} else if (passed_min==true) {
			if ((yvals[j+1]+yvals[j]) > 2*ymax) {
				jmax = j-1;
				break;
			}
		}
	}
	if ((imin==imax) or (jmin==jmax)) die("window for centroid calculation has zero size");
	double centroid_x=0, centroid_y=0, centroid_err_x=0, centroid_err_y=0, total_flux=0;
	int np=0;
	double xm,ym;
	for (j=jmin; j <= jmax; j++) {
		for (i=imin; i <= imax; i++) {
			if (surface_brightness[i][j] > sb_threshold) {
				xm = 0.5*(xvals[i+1]+xvals[i]);
				ym = 0.5*(yvals[j+1]+yvals[j]);
				centroid_x += xm*surface_brightness[i][j];
				centroid_y += ym*surface_brightness[i][j];
				//centroid_err_x += xm*xm*surface_brightness[i][j];
				//centroid_err_y += ym*ym*surface_brightness[i][j];
				total_flux += surface_brightness[i][j];
				np++;
			}
		}
	}
	if (total_flux==0) die("Zero pixels are above the stated surface brightness threshold for calculating image centroid");
	//double avg_signal = total_flux / np;
	//cout << "Average signal: " << avg_signal << endl;
	centroid_x /= total_flux;
	centroid_y /= total_flux;
	double centroid_err_x_sb=0, centroid_err_y_sb=0;
	for (j=jmin; j <= jmax; j++) {
		for (i=imin; i <= imax; i++) {
			if (surface_brightness[i][j] > sb_threshold) {
				xm = 0.5*(xvals[i+1]+xvals[i]);
				ym = 0.5*(yvals[j+1]+yvals[j]);
				centroid_err_x_sb += SQR(xm-centroid_x);
				centroid_err_y_sb += SQR(ym-centroid_y);
			}
		}
	}

	// Finding an error based on the second moment seems flawed, since with enough pixels the centroid should be known very well.
	// For now, we choose the pixel size to give the error.
	//centroid_err_x /= total_flux;
	//centroid_err_y /= total_flux;
	//centroid_err_x = sqrt(centroid_err_x - SQR(centroid_x));
	//centroid_err_y = sqrt(centroid_err_y - SQR(centroid_y));
	centroid_err_x_sb = sqrt(centroid_err_x_sb)*pixel_error/total_flux;
	centroid_err_y_sb = sqrt(centroid_err_y_sb)*pixel_error/total_flux;
	centroid_err_x = xvals[1] - xvals[0];
	centroid_err_y = yvals[1] - yvals[0];
	centroid_err_x = sqrt(SQR(centroid_err_x) + SQR(centroid_err_x_sb));
	centroid_err_y = sqrt(SQR(centroid_err_y) + SQR(centroid_err_y_sb));
	//cout << "err_x_sb=" << centroid_err_x_sb << ", err_x=" << centroid_err_x << ", err_y_sb=" << centroid_err_y_sb << ", err_y=" << centroid_err_y << endl;
	double centroid_err = dmax(centroid_err_x,centroid_err_y);
	double flux_err = pixel_error / sqrt(np);
	//cout << "centroid = (" << centroid_x << "," << centroid_y << "), err=(" << centroid_err_x << "," << centroid_err_y << "), flux = " << total_flux << ", flux_err = " << flux_err << endl;
	lensvector pos; pos[0] = centroid_x; pos[1] = centroid_y;
	point_image_data->add_image(pos,centroid_err,total_flux,flux_err,0,0);
}

void ImagePixelData::plot_surface_brightness(string outfile_root)
{
	string sb_filename = outfile_root + ".dat";
	string x_filename = outfile_root + ".x";
	string y_filename = outfile_root + ".y";

	ofstream pixel_image_file(sb_filename.c_str());
	ofstream pixel_xvals(x_filename.c_str());
	ofstream pixel_yvals(y_filename.c_str());
	pixel_image_file << setiosflags(ios::scientific);
	int i,j;
	for (int i=0; i <= npixels_x; i++) {
		pixel_xvals << xvals[i] << endl;
	}
	for (int j=0; j <= npixels_y; j++) {
		pixel_yvals << yvals[j] << endl;
	}	
	for (j=0; j < npixels_y; j++) {
		for (i=0; i < npixels_x; i++) {
			if ((require_fit == NULL) or (require_fit[i][j])) {
				//if (abs(surface_brightness[i][j]) > lens->noise_threshold*lens->data_pixel_noise)
					pixel_image_file << surface_brightness[i][j];
				//else
					//pixel_image_file << "0";
			} else {
				pixel_image_file << "0";
			}
			if (i < npixels_x-1) pixel_image_file << " ";
		}
		pixel_image_file << endl;
	}
}

ImagePixelGrid::ImagePixelGrid(Lens* lens_in, RayTracingMethod method, double xmin_in, double xmax_in, double ymin_in, double ymax_in, int x_N_in, int y_N_in) : lens(lens_in), xmin(xmin_in), xmax(xmax_in), ymin(ymin_in), ymax(ymax_in), x_N(x_N_in), y_N(y_N_in)
{
	ray_tracing_method = method;
	xy_N = x_N*y_N;
	n_active_pixels = 0;
	corner_pts = new lensvector*[x_N+1];
	corner_sourcepts = new lensvector*[x_N+1];
	center_pts = new lensvector*[x_N];
	center_sourcepts = new lensvector*[x_N];
	center_magnifications = new double*[x_N];
	maps_to_source_pixel = new bool*[x_N];
	pixel_index = new int*[x_N];
	mapped_source_pixels = new vector<SourcePixelGrid*>*[x_N];
	surface_brightness = new double*[x_N];
	source_plane_triangle1_area = new double*[x_N];
	source_plane_triangle2_area = new double*[x_N];
	int i,j;
	for (i=0; i <= x_N; i++) {
		corner_pts[i] = new lensvector[y_N+1];
		corner_sourcepts[i] = new lensvector[y_N+1];
	}
	for (i=0; i < x_N; i++) {
		center_pts[i] = new lensvector[y_N];
		center_sourcepts[i] = new lensvector[y_N];
		center_magnifications[i] = new double[y_N];
		maps_to_source_pixel[i] = new bool[y_N];
		pixel_index[i] = new int[y_N];
		surface_brightness[i] = new double[y_N];
		source_plane_triangle1_area[i] = new double[y_N];
		source_plane_triangle2_area[i] = new double[y_N];
		mapped_source_pixels[i] = new vector<SourcePixelGrid*>[y_N];
	}
	zfactor = lens->reference_zfactor;

	pixel_xlength = (xmax-xmin)/x_N;
	pixel_ylength = (ymax-ymin)/y_N;
	triangle_area = 0.5*pixel_xlength*pixel_ylength;

#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		double x,y;
		lensvector d1,d2,d3,d4;
		#pragma omp for private(i,j) schedule(dynamic)
		for (j=0; j <= y_N; j++) {
			y = ymin + j*pixel_ylength;
			for (i=0; i <= x_N; i++) {
				x = xmin + i*pixel_xlength;
				if ((i < x_N) and (j < y_N)) {
					center_pts[i][j][0] = x + 0.5*pixel_xlength;
					center_pts[i][j][1] = y + 0.5*pixel_ylength;
					lens->find_sourcept(center_pts[i][j],center_sourcepts[i][j],thread,zfactor);
					center_magnifications[i][j] = abs(lens->magnification(center_pts[i][j],thread,zfactor));
				}
				corner_pts[i][j][0] = x;
				corner_pts[i][j][1] = y;
				lens->find_sourcept(corner_pts[i][j],corner_sourcepts[i][j],thread,zfactor);
			}
		}
		#pragma omp for private(i,j) schedule(dynamic)
		for (j=0; j < y_N; j++) {
			for (i=0; i < x_N; i++) {
				d1[0] = corner_sourcepts[i][j][0] - corner_sourcepts[i+1][j][0];
				d1[1] = corner_sourcepts[i][j][1] - corner_sourcepts[i+1][j][1];
				d2[0] = corner_sourcepts[i][j+1][0] - corner_sourcepts[i][j][0];
				d2[1] = corner_sourcepts[i][j+1][1] - corner_sourcepts[i][j][1];
				d3[0] = corner_sourcepts[i+1][j+1][0] - corner_sourcepts[i][j+1][0];
				d3[1] = corner_sourcepts[i+1][j+1][1] - corner_sourcepts[i][j+1][1];
				d4[0] = corner_sourcepts[i+1][j][0] - corner_sourcepts[i+1][j+1][0];
				d4[1] = corner_sourcepts[i+1][j][1] - corner_sourcepts[i+1][j+1][1];
				source_plane_triangle1_area[i][j] = 0.5*abs(d1 ^ d2);
				source_plane_triangle2_area[i][j] = 0.5*abs(d3 ^ d4);
			}
		}
	}
#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for creating and ray-tracing image pixel grid: " << wtime << endl;
	}
#endif
	fit_to_data = NULL;
}

ImagePixelGrid::ImagePixelGrid(Lens* lens_in, ImagePixelGrid* input_pixel_grid) : lens(lens_in)
{
	ray_tracing_method = input_pixel_grid->ray_tracing_method;
	xmin = input_pixel_grid->xmin;
	xmax = input_pixel_grid->xmax;
	ymin = input_pixel_grid->ymin;
	ymax = input_pixel_grid->ymax;
	x_N = input_pixel_grid->x_N;
	y_N = input_pixel_grid->y_N;
	xy_N = x_N*y_N;
	pixel_noise = input_pixel_grid->pixel_noise;
	n_active_pixels = input_pixel_grid->n_active_pixels;
	corner_pts = new lensvector*[x_N+1];
	corner_sourcepts = new lensvector*[x_N+1];
	center_pts = new lensvector*[x_N];
	center_sourcepts = new lensvector*[x_N];
	center_magnifications = new double*[x_N];
	maps_to_source_pixel = new bool*[x_N];
	pixel_index = new int*[x_N];
	mapped_source_pixels = new vector<SourcePixelGrid*>*[x_N];
	surface_brightness = new double*[x_N];
	source_plane_triangle1_area = new double*[x_N];
	source_plane_triangle2_area = new double*[x_N];
	int i,j;
	for (i=0; i <= x_N; i++) {
		corner_pts[i] = new lensvector[y_N+1];
		corner_sourcepts[i] = new lensvector[y_N+1];
	}
	for (i=0; i < x_N; i++) {
		center_pts[i] = new lensvector[y_N];
		center_sourcepts[i] = new lensvector[y_N];
		center_magnifications[i] = new double[y_N];
		maps_to_source_pixel[i] = new bool[y_N];
		pixel_index[i] = new int[y_N];
		surface_brightness[i] = new double[y_N];
		source_plane_triangle1_area[i] = new double[y_N];
		source_plane_triangle2_area[i] = new double[y_N];
		mapped_source_pixels[i] = new vector<SourcePixelGrid*>[y_N];
		for (j=0; j < y_N; j++)
			maps_to_source_pixel[i][j] = input_pixel_grid->maps_to_source_pixel[i][j];
	}
	zfactor = lens->reference_zfactor;

	triangle_area = 0.5*pixel_xlength*pixel_ylength;
	lensvector d1,d2,d3,d4;

	for (j=0; j <= y_N; j++) {
		for (i=0; i <= x_N; i++) {
			if ((i < x_N) and (j < y_N)) {
				center_pts[i][j][0] = input_pixel_grid->center_pts[i][j][0];
				center_pts[i][j][1] = input_pixel_grid->center_pts[i][j][1];
				center_sourcepts[i][j] = input_pixel_grid->center_sourcepts[i][j];
				center_magnifications[i][j] = input_pixel_grid->center_magnifications[i][j];
				surface_brightness[i][j] = input_pixel_grid->surface_brightness[i][j];
				max_sb=input_pixel_grid->max_sb;
			}
			corner_pts[i][j][0] = input_pixel_grid->corner_pts[i][j][0];
			corner_pts[i][j][1] = input_pixel_grid->corner_pts[i][j][1];
			corner_sourcepts[i][j] = input_pixel_grid->corner_sourcepts[i][j];
		}
	}
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			d1[0] = corner_sourcepts[i][j][0] - corner_sourcepts[i+1][j][0];
			d1[1] = corner_sourcepts[i][j][1] - corner_sourcepts[i+1][j][1];
			d2[0] = corner_sourcepts[i][j+1][0] - corner_sourcepts[i][j][0];
			d2[1] = corner_sourcepts[i][j+1][1] - corner_sourcepts[i][j][1];
			d3[0] = corner_sourcepts[i+1][j+1][0] - corner_sourcepts[i][j+1][0];
			d3[1] = corner_sourcepts[i+1][j+1][1] - corner_sourcepts[i][j+1][1];
			d4[0] = corner_sourcepts[i+1][j][0] - corner_sourcepts[i+1][j+1][0];
			d4[1] = corner_sourcepts[i+1][j][1] - corner_sourcepts[i+1][j+1][1];
			source_plane_triangle1_area[i][j] = 0.5*abs(d1 ^ d2);
			source_plane_triangle2_area[i][j] = 0.5*abs(d3 ^ d4);
		}
	}
	fit_to_data = NULL;
}

ImagePixelGrid::ImagePixelGrid(Lens* lens_in, RayTracingMethod method, ImagePixelData& pixel_data) : lens(lens_in)
{
	ray_tracing_method = method;
	pixel_data.get_grid_params(xmin,xmax,ymin,ymax,x_N,y_N);
	xy_N = x_N*y_N;
	n_active_pixels = 0;
	corner_pts = new lensvector*[x_N+1];
	corner_sourcepts = new lensvector*[x_N+1];
	center_pts = new lensvector*[x_N];
	center_sourcepts = new lensvector*[x_N];
	center_magnifications = new double*[x_N];
	maps_to_source_pixel = new bool*[x_N];
	fit_to_data = new bool*[x_N];
	pixel_index = new int*[x_N];
	mapped_source_pixels = new vector<SourcePixelGrid*>*[x_N];
	surface_brightness = new double*[x_N];
	source_plane_triangle1_area = new double*[x_N];
	source_plane_triangle2_area = new double*[x_N];
	int i,j;
	for (i=0; i <= x_N; i++) {
		corner_pts[i] = new lensvector[y_N+1];
		corner_sourcepts[i] = new lensvector[y_N+1];
	}
	for (i=0; i < x_N; i++) {
		center_pts[i] = new lensvector[y_N];
		center_sourcepts[i] = new lensvector[y_N];
		center_magnifications[i] = new double[y_N];
		maps_to_source_pixel[i] = new bool[y_N];
		fit_to_data[i] = new bool[y_N];
		pixel_index[i] = new int[y_N];
		surface_brightness[i] = new double[y_N];
		source_plane_triangle1_area[i] = new double[y_N];
		source_plane_triangle2_area[i] = new double[y_N];
		mapped_source_pixels[i] = new vector<SourcePixelGrid*>[y_N];
	}
	zfactor = lens->reference_zfactor;

	pixel_xlength = (xmax-xmin)/x_N;
	pixel_ylength = (ymax-ymin)/y_N;
	max_sb = -1e30;
	triangle_area = 0.5*pixel_xlength*pixel_ylength;

#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		double x,y;
		lensvector d1,d2,d3,d4;
		#pragma omp for private(i,j) schedule(dynamic)
		for (j=0; j <= y_N; j++) {
			y = pixel_data.yvals[j];
			for (i=0; i <= x_N; i++) {
				x = pixel_data.xvals[i];
				if ((i < x_N) and (j < y_N)) {
					center_pts[i][j][0] = x + 0.5*pixel_xlength;
					center_pts[i][j][1] = y + 0.5*pixel_ylength;
					lens->find_sourcept(center_pts[i][j],center_sourcepts[i][j],thread,zfactor);
					center_magnifications[i][j] = abs(lens->magnification(center_pts[i][j],thread,zfactor));
					surface_brightness[i][j] = pixel_data.surface_brightness[i][j];
					fit_to_data[i][j] = pixel_data.require_fit[i][j];
					if (surface_brightness[i][j] > max_sb) max_sb=surface_brightness[i][j];
				}
				corner_pts[i][j][0] = x;
				corner_pts[i][j][1] = y;
				lens->find_sourcept(corner_pts[i][j],corner_sourcepts[i][j],thread,zfactor);
			}
		}
		#pragma omp for private(i,j) schedule(dynamic)
		for (j=0; j < y_N; j++) {
			for (i=0; i < x_N; i++) {
				d1[0] = corner_sourcepts[i][j][0] - corner_sourcepts[i+1][j][0];
				d1[1] = corner_sourcepts[i][j][1] - corner_sourcepts[i+1][j][1];
				d2[0] = corner_sourcepts[i][j+1][0] - corner_sourcepts[i][j][0];
				d2[1] = corner_sourcepts[i][j+1][1] - corner_sourcepts[i][j][1];
				d3[0] = corner_sourcepts[i+1][j+1][0] - corner_sourcepts[i][j+1][0];
				d3[1] = corner_sourcepts[i+1][j+1][1] - corner_sourcepts[i][j+1][1];
				d4[0] = corner_sourcepts[i+1][j][0] - corner_sourcepts[i+1][j+1][0];
				d4[1] = corner_sourcepts[i+1][j][1] - corner_sourcepts[i+1][j+1][1];
				source_plane_triangle1_area[i][j] = 0.5*abs(d1 ^ d2);
				source_plane_triangle2_area[i][j] = 0.5*abs(d3 ^ d4);
			}
		}
	}
#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for creating and ray-tracing image pixel grid: " << wtime << endl;
	}
#endif
}

ImagePixelGrid::ImagePixelGrid(double zfactor_in, RayTracingMethod method, ImagePixelData& pixel_data)
{
	ray_tracing_method = method;
	pixel_data.get_grid_params(xmin,xmax,ymin,ymax,x_N,y_N);
	xy_N = x_N*y_N;
	n_active_pixels = 0;
	corner_pts = new lensvector*[x_N+1];
	corner_sourcepts = new lensvector*[x_N+1];
	center_pts = new lensvector*[x_N];
	center_sourcepts = new lensvector*[x_N];
	center_magnifications = new double*[x_N];
	fit_to_data = new bool*[x_N];
	maps_to_source_pixel = new bool*[x_N];
	pixel_index = new int*[x_N];
	mapped_source_pixels = new vector<SourcePixelGrid*>*[x_N];
	surface_brightness = new double*[x_N];
	source_plane_triangle1_area = new double*[x_N];
	source_plane_triangle2_area = new double*[x_N];
	int i,j;
	for (i=0; i <= x_N; i++) {
		corner_pts[i] = new lensvector[y_N+1];
		corner_sourcepts[i] = new lensvector[y_N+1];
	}
	for (i=0; i < x_N; i++) {
		center_pts[i] = new lensvector[y_N];
		center_sourcepts[i] = new lensvector[y_N];
		center_magnifications[i] = new double[y_N];
		maps_to_source_pixel[i] = new bool[y_N];
		fit_to_data[i] = new bool[y_N];
		pixel_index[i] = new int[y_N];
		surface_brightness[i] = new double[y_N];
		source_plane_triangle1_area[i] = new double[y_N];
		source_plane_triangle2_area[i] = new double[y_N];
		mapped_source_pixels[i] = new vector<SourcePixelGrid*>[y_N];
	}
	zfactor = zfactor_in;

	pixel_xlength = (xmax-xmin)/x_N;
	pixel_ylength = (ymax-ymin)/y_N;
	max_sb = -1e30;
	triangle_area = 0.5*pixel_xlength*pixel_ylength;

	double x,y;
	for (j=0; j <= y_N; j++) {
		y = pixel_data.yvals[j];
		for (i=0; i <= x_N; i++) {
			x = pixel_data.xvals[i];
			if ((i < x_N) and (j < y_N)) {
				center_pts[i][j][0] = x + 0.5*pixel_xlength;
				center_pts[i][j][1] = y + 0.5*pixel_ylength;
				surface_brightness[i][j] = pixel_data.surface_brightness[i][j];
				fit_to_data[i][j] = pixel_data.require_fit[i][j];
				if (surface_brightness[i][j] > max_sb) max_sb=surface_brightness[i][j];
			}
			corner_pts[i][j][0] = x;
			corner_pts[i][j][1] = y;
		}
	}
}

void ImagePixelGrid::set_fit_window(ImagePixelData& pixel_data)
{
	if ((x_N != pixel_data.npixels_x) or (y_N != pixel_data.npixels_y)) {
		warn("Number of data pixels does not match specified number of image pixels; cannot activate fit window");
		return;
	}
	int i,j;
	if (fit_to_data==NULL) {
		fit_to_data = new bool*[x_N];
		for (i=0; i < x_N; i++) fit_to_data[i] = new bool[y_N];
	}
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			fit_to_data[i][j] = pixel_data.require_fit[i][j];
		}
	}
}

void ImagePixelGrid::include_all_pixels()
{
	int i,j;
	if (fit_to_data==NULL) {
		fit_to_data = new bool*[x_N];
		for (i=0; i < x_N; i++) fit_to_data[i] = new bool[y_N];
	}
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			fit_to_data[i][j] = true;
		}
	}
}

void ImagePixelGrid::redo_lensing_calculations()
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*(lens->group_comm), *(lens->mpi_group), &sub_comm);
#endif

#ifdef USE_OPENMP
	double wtime0, wtime;
	if (lens->show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	n_active_pixels = 0;
	int i,j,n,n_cell,n_yp;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++)
			mapped_source_pixels[i][j].clear();
	}
	long int ntot_corners = (x_N+1)*(y_N+1);
	long int ntot_cells = x_N*y_N;
	double *defx_corners, *defy_corners, *defx_centers, *defy_centers, *area_tri1, *area_tri2;
	defx_corners = new double[ntot_corners];
	defy_corners = new double[ntot_corners];
	defx_centers = new double[ntot_cells];
	defy_centers = new double[ntot_cells];
	area_tri1 = new double[ntot_cells];
	area_tri2 = new double[ntot_cells];

	int mpi_chunk, mpi_start, mpi_end;
	mpi_chunk = ntot_corners / lens->group_np;
	mpi_start = lens->group_id*mpi_chunk;
	if (lens->group_id == lens->group_np-1) mpi_chunk += (ntot_corners % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end = mpi_start + mpi_chunk;

	int mpi_chunk2, mpi_start2, mpi_end2;
	mpi_chunk2 = ntot_cells / lens->group_np;
	mpi_start2 = lens->group_id*mpi_chunk2;
	if (lens->group_id == lens->group_np-1) mpi_chunk2 += (ntot_cells % lens->group_np); // assign the remainder elements to the last mpi process
	mpi_end2 = mpi_start2 + mpi_chunk2;

	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
		lensvector d1,d2,d3,d4;
		#pragma omp for private(n,i,j) schedule(dynamic)
		for (n=mpi_start; n < mpi_end; n++) {
			j = n / (x_N+1);
			i = n % (x_N+1);
			lens->find_sourcept(corner_pts[i][j],defx_corners[n],defy_corners[n],thread,zfactor);
		}
#ifdef USE_MPI
		#pragma omp master
		{
			int id, chunk, start;
			for (id=0; id < lens->group_np; id++) {
				chunk = ntot_corners / lens->group_np;
				start = id*chunk;
				if (id == lens->group_np-1) chunk += (ntot_corners % lens->group_np); // assign the remainder elements to the last mpi process
				MPI_Bcast(defx_corners+start,chunk,MPI_DOUBLE,id,sub_comm);
				MPI_Bcast(defy_corners+start,chunk,MPI_DOUBLE,id,sub_comm);
			}
		}
		#pragma omp barrier
#endif
		#pragma omp for private(n_cell,i,j,n,n_yp) schedule(dynamic)
		for (n_cell=mpi_start2; n_cell < mpi_end2; n_cell++) {
			j = n_cell / x_N;
			i = n_cell % x_N;
			lens->find_sourcept(center_pts[i][j],defx_centers[n_cell],defy_centers[n_cell],thread,zfactor);
			center_magnifications[i][j] = abs(lens->magnification(center_pts[i][j],thread,zfactor));

			n = j*(x_N+1)+i;
			n_yp = (j+1)*(x_N+1)+i;
			d1[0] = defx_corners[n] - defx_corners[n+1];
			d1[1] = defy_corners[n] - defy_corners[n+1];
			d2[0] = defx_corners[n_yp] - defx_corners[n];
			d2[1] = defy_corners[n_yp] - defy_corners[n];
			d3[0] = defx_corners[n_yp+1] - defx_corners[n_yp];
			d3[1] = defy_corners[n_yp+1] - defy_corners[n_yp];
			d4[0] = defx_corners[n+1] - defx_corners[n_yp+1];
			d4[1] = defy_corners[n+1] - defy_corners[n_yp+1];
			area_tri1[n_cell] = 0.5*abs(d1 ^ d2);
			area_tri2[n_cell] = 0.5*abs(d3 ^ d4);
		}
	}
#ifdef USE_MPI
	int id, chunk, start;
	for (id=0; id < lens->group_np; id++) {
		chunk = ntot_cells / lens->group_np;
		start = id*chunk;
		if (id == lens->group_np-1) chunk += (ntot_cells % lens->group_np); // assign the remainder elements to the last mpi process
		MPI_Bcast(defx_centers+start,chunk,MPI_DOUBLE,id,sub_comm);
		MPI_Bcast(defy_centers+start,chunk,MPI_DOUBLE,id,sub_comm);
		MPI_Bcast(area_tri1+start,chunk,MPI_DOUBLE,id,sub_comm);
		MPI_Bcast(area_tri2+start,chunk,MPI_DOUBLE,id,sub_comm);
	}
	MPI_Comm_free(&sub_comm);
#endif
	for (n=0; n < ntot_corners; n++) {
		j = n / (x_N+1);
		i = n % (x_N+1);
		corner_sourcepts[i][j][0] = defx_corners[n];
		corner_sourcepts[i][j][1] = defy_corners[n];
		if ((i < x_N) and (j < y_N)) {
			n_cell = j*x_N+i;
			source_plane_triangle1_area[i][j] = area_tri1[n_cell];
			source_plane_triangle2_area[i][j] = area_tri2[n_cell];
			center_sourcepts[i][j][0] = defx_centers[n_cell];
			center_sourcepts[i][j][1] = defy_centers[n_cell];
		}
	}

#ifdef USE_OPENMP
	if (lens->show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (lens->mpi_id==0) cout << "Wall time for ray-tracing image pixel grid: " << wtime << endl;
	}
#endif
	delete[] defx_corners;
	delete[] defy_corners;
	delete[] defx_centers;
	delete[] defy_centers;
	delete[] area_tri1;
	delete[] area_tri2;
}

bool ImagePixelData::test_if_in_fit_region(const double& x, const double& y)
{
	// it would be faster to just use division to figure out which pixel it's in, but this is good enough
	int i,j;
	for (j=0; j <= npixels_y; j++) {
		if ((yvals[j] <= y) and (yvals[j+1] > y)) {
			for (i=0; i <= npixels_x; i++) {
				if ((xvals[i] <= x) and (xvals[i+1] > x)) {
					if (require_fit[i][j] == true) return true;
					else break;
				}
			}
		}
	}
	return false;
}

void ImagePixelGrid::plot_center_pts_source_plane()
{
	ofstream outfile("pixel_maps.dat");
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			outfile << center_sourcepts[i][j][0] << " " << center_sourcepts[i][j][1] << endl;
		}
	}
}

double ImagePixelGrid::calculate_signal_to_noise(const double& pixel_noise_sig)
{
	// NOTE: This function should be called *before* adding noise to the image.
	double sbmax=-1e30;
	static const double signal_threshold_frac = 1e-1;
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if (surface_brightness[i][j] > sbmax) sbmax = surface_brightness[i][j];
		}
	}
	double signal_mean=0;
	int npixels=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if (surface_brightness[i][j] > signal_threshold_frac*sbmax) {
				signal_mean += surface_brightness[i][j];
				npixels++;
			}
		}
	}
	if (npixels > 0) signal_mean /= npixels;
	return signal_mean / pixel_noise_sig;
}

void ImagePixelGrid::add_pixel_noise(const double& pixel_noise_sig)
{
	if (surface_brightness == NULL) die("surface brightness pixel map has not been loaded");
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			surface_brightness[i][j] += pixel_noise_sig*lens->NormalDeviate();
		}
	}
	pixel_noise = pixel_noise_sig;
}

void ImagePixelGrid::find_optimal_sourcegrid(double& sourcegrid_xmin, double& sourcegrid_xmax, double& sourcegrid_ymin, double& sourcegrid_ymax, const double &sourcegrid_limit_xmin, const double &sourcegrid_limit_xmax, const double &sourcegrid_limit_ymin, const double& sourcegrid_limit_ymax)
{
	if (surface_brightness == NULL) die("surface brightness pixel map has not been loaded");
	double threshold = lens->noise_threshold*pixel_noise;
	int i,j;
	sourcegrid_xmin=1e30;
	sourcegrid_xmax=-1e30;
	sourcegrid_ymin=1e30;
	sourcegrid_ymax=-1e30;
	int ii,jj,il,ih,jl,jh,nn;
	double sbavg;
	int window_size=2;
	for (i=0; i < x_N; i++) {
		for (j=0; j < y_N; j++) {
			if (fit_to_data[i][j]) {
				sbavg=0;
				nn=0;
				il=i-window_size;
				ih=i+window_size;
				jl=j-window_size;
				jh=j+window_size;
				if (il<0) il=0;
				if (ih>x_N-1) ih=x_N-1;
				if (jl<0) jl=0;
				if (jh>y_N-1) jh=y_N-1;
				for (ii=il; ii <= ih; ii++) {
					for (jj=jl; jj <= jh; jj++) {
						sbavg += surface_brightness[ii][jj];
						nn++;
					}
				}
				sbavg /= nn;
				if (sbavg > threshold) {
					if (center_sourcepts[i][j][0] < sourcegrid_xmin) {
						if (center_sourcepts[i][j][0] > sourcegrid_limit_xmin) sourcegrid_xmin = center_sourcepts[i][j][0];
						else if (sourcegrid_xmin > sourcegrid_limit_xmin) sourcegrid_xmin = sourcegrid_limit_xmin;
					}
					if (center_sourcepts[i][j][0] > sourcegrid_xmax) {
						if (center_sourcepts[i][j][0] < sourcegrid_limit_xmax) sourcegrid_xmax = center_sourcepts[i][j][0];
						else if (sourcegrid_xmax < sourcegrid_limit_xmax) sourcegrid_xmax = sourcegrid_limit_xmax;
					}
					if (center_sourcepts[i][j][1] < sourcegrid_ymin) {
						if (center_sourcepts[i][j][1] > sourcegrid_limit_ymin) sourcegrid_ymin = center_sourcepts[i][j][1];
						else if (sourcegrid_ymin > sourcegrid_limit_ymin) sourcegrid_ymin = sourcegrid_limit_ymin;
					}
					if (center_sourcepts[i][j][1] > sourcegrid_ymax) {
						if (center_sourcepts[i][j][1] < sourcegrid_limit_ymax) sourcegrid_ymax = center_sourcepts[i][j][1];
						else if (sourcegrid_ymax < sourcegrid_limit_ymax) sourcegrid_ymax = sourcegrid_limit_ymax;
					}
				}
			}
		}
	}
	// Now let's make the box slightly wider just to be sure
	double xwidth_adj = 0.3*(sourcegrid_xmax-sourcegrid_xmin);
	double ywidth_adj = 0.3*(sourcegrid_ymax-sourcegrid_ymin);
	sourcegrid_xmin -= xwidth_adj/2;
	sourcegrid_xmax += xwidth_adj/2;
	sourcegrid_ymin -= ywidth_adj/2;
	sourcegrid_ymax += ywidth_adj/2;
}

void ImagePixelGrid::fill_surface_brightness_vector()
{
	int column_j = 0;
	int i,j;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if (maps_to_source_pixel[i][j]) {
				lens->image_surface_brightness[column_j++] = surface_brightness[i][j];
			}
		}
	}
}

void ImagePixelGrid::plot_grid(string filename, bool show_inactive_pixels)
{
	int i,j;
	ofstream outfile(filename.c_str());
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((show_inactive_pixels) or (maps_to_source_pixel[i][j])) {
				if (ray_tracing_method == Area_Overlap) {
					outfile << corner_sourcepts[i][j][0] << " " << corner_sourcepts[i][j][1] << endl;
					outfile << corner_sourcepts[i+1][j][0] << " " << corner_sourcepts[i+1][j][1] << endl;
					outfile << corner_sourcepts[i+1][j+1][0] << " " << corner_sourcepts[i+1][j+1][1] << endl;
					outfile << corner_sourcepts[i][j+1][0] << " " << corner_sourcepts[i][j+1][1] << endl;
					outfile << corner_sourcepts[i][j][0] << " " << corner_sourcepts[i][j][1] << endl;
					outfile << endl;
				} else {
					outfile << center_sourcepts[i][j][0] << " " << center_sourcepts[i][j][1] << endl;
				}
			}
		}
	}
}

void ImagePixelGrid::find_optimal_sourcegrid_npixels(double pixel_fraction, double srcgrid_xmin, double srcgrid_xmax, double srcgrid_ymin, double srcgrid_ymax, int& nsrcpixel_x, int& nsrcpixel_y, int& n_expected_active_pixels)
{
	int i,j,count=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((fit_to_data==NULL) or (fit_to_data[i][j])) {
				if ((center_sourcepts[i][j][0] > srcgrid_xmin) and (center_sourcepts[i][j][0] < srcgrid_xmax) and (center_sourcepts[i][j][1] > srcgrid_ymin) and (center_sourcepts[i][j][1] < srcgrid_ymax)) {
					count++;
				}
			}
		}
	}
	double dx = srcgrid_xmax-srcgrid_xmin;
	double dy = srcgrid_ymax-srcgrid_ymin;
	nsrcpixel_x = (int) sqrt(pixel_fraction*count*dx/dy);
	nsrcpixel_y = (int) nsrcpixel_x*dy/dx;
	n_expected_active_pixels = count;
}

void ImagePixelGrid::find_optimal_firstlevel_sourcegrid_npixels(double srcgrid_xmin, double srcgrid_xmax, double srcgrid_ymin, double srcgrid_ymax, int& nsrcpixel_x, int& nsrcpixel_y, int& n_expected_active_pixels)
{
	// this algorithm assumes an adaptive grid, so that higher magnification regions will be subgridded
	double lowest_magnification = 1e30;
	double average_magnification = 0;
	int i,j,count=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((fit_to_data==NULL) or (fit_to_data[i][j])) {
				if ((center_sourcepts[i][j][0] > srcgrid_xmin) and (center_sourcepts[i][j][0] < srcgrid_xmax) and (center_sourcepts[i][j][1] > srcgrid_ymin) and (center_sourcepts[i][j][1] < srcgrid_ymax)) {
					count++;
				}
			}
		}
	}

	double pixel_area, source_lowlevel_pixel_area, dx, dy, srcgrid_area, srcgrid_firstlevel_npixels;
	pixel_area = pixel_xlength * pixel_ylength;
	source_lowlevel_pixel_area = pixel_area / (1.3*lens->pixel_magnification_threshold);
	dx = srcgrid_xmax-srcgrid_xmin;
	dy = srcgrid_ymax-srcgrid_ymin;
	srcgrid_area = dx*dy;
	srcgrid_firstlevel_npixels = dx*dy/source_lowlevel_pixel_area;
	nsrcpixel_x = (int) sqrt(srcgrid_firstlevel_npixels*dx/dy);
	nsrcpixel_y = (int) nsrcpixel_x*dy/dx;
	int srcgrid_npixels = nsrcpixel_x*nsrcpixel_y;
	n_expected_active_pixels = count;
}

void ImagePixelGrid::assign_required_data_pixels(double srcgrid_xmin, double srcgrid_xmax, double srcgrid_ymin, double srcgrid_ymax, int& count, ImagePixelData *data_in)
{
	int i,j;
	count=0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((center_sourcepts[i][j][0] > srcgrid_xmin) and (center_sourcepts[i][j][0] < srcgrid_xmax) and (center_sourcepts[i][j][1] > srcgrid_ymin) and (center_sourcepts[i][j][1] < srcgrid_ymax)) {
				data_in->require_fit[i][j] = true;
				count++;
			}
			else {
				data_in->require_fit[i][j] = false;
			}
		}
	}
}

int ImagePixelGrid::count_nonzero_source_pixel_mappings()
{
	int tot=0;
	int i,j,img_index;
	for (img_index=0; img_index < lens->image_npixels; img_index++) {
		i = lens->active_image_pixel_i[img_index];
		j = lens->active_image_pixel_j[img_index];
		tot += mapped_source_pixels[i][j].size();
	}
	return tot;
}

void ImagePixelGrid::assign_image_mapping_flags()
{
	int i,j;
	n_active_pixels = 0;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			mapped_source_pixels[i][j].clear();
			maps_to_source_pixel[i][j] = false;
		}
	}
	if (ray_tracing_method == Area_Overlap)
	{
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			lensvector *corners[4];
			#pragma omp for private(i,j,corners) schedule(dynamic)
			for (j=0; j < y_N; j++) {
				for (i=0; i < x_N; i++) {
					if ((fit_to_data == NULL) or (fit_to_data[i][j])) {
						corners[0] = &corner_sourcepts[i][j];
						corners[1] = &corner_sourcepts[i][j+1];
						corners[2] = &corner_sourcepts[i+1][j];
						corners[3] = &corner_sourcepts[i+1][j+1];
						if (source_pixel_grid->assign_source_mapping_flags_overlap(corners,mapped_source_pixels[i][j],thread)==true) {
							maps_to_source_pixel[i][j] = true;
							#pragma omp atomic
							n_active_pixels++;
						} else
							maps_to_source_pixel[i][j] = false;
					}
				}
			}
		}
	}
	else if (ray_tracing_method == Interpolate)
	{
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			#pragma omp for private(i,j) schedule(dynamic)
			for (j=0; j < y_N; j++) {
				for (i=0; i < x_N; i++) {
					if ((fit_to_data == NULL) or (fit_to_data[i][j])) {
						if (source_pixel_grid->assign_source_mapping_flags_interpolate(center_sourcepts[i][j],mapped_source_pixels[i][j],thread,i,j)==true) {
							maps_to_source_pixel[i][j] = true;
							#pragma omp atomic
							n_active_pixels++;
						} else
							maps_to_source_pixel[i][j] = false;
					}
				}
			}
		}
	}
}

void ImagePixelGrid::find_surface_brightness()
{
	if (ray_tracing_method == Area_Overlap) {
		lensvector **corners = new lensvector*[4];
		int i,j;
		for (j=0; j < y_N; j++) {
			for (i=0; i < x_N; i++) {
				corners[0] = &corner_sourcepts[i][j];
				corners[1] = &corner_sourcepts[i][j+1];
				corners[2] = &corner_sourcepts[i+1][j];
				corners[3] = &corner_sourcepts[i+1][j+1];
				surface_brightness[i][j] = source_pixel_grid->find_lensed_surface_brightness_overlap(corners,0);
			}
		}
		delete[] corners;
	}
	else if (ray_tracing_method == Interpolate) {
		int i,j;
		for (j=0; j < y_N; j++) {
			for (i=0; i < x_N; i++) {
				surface_brightness[i][j] = source_pixel_grid->find_lensed_surface_brightness_interpolate(center_sourcepts[i][j],0);
			}
		}
	}
}

void ImagePixelGrid::plot_surface_brightness(string outfile_root, bool plot_residual)
{
	string sb_filename = outfile_root + ".dat";
	string x_filename = outfile_root + ".x";
	string y_filename = outfile_root + ".y";

	ofstream pixel_image_file(sb_filename.c_str());
	ofstream pixel_xvals(x_filename.c_str());
	ofstream pixel_yvals(y_filename.c_str());
	pixel_image_file << setiosflags(ios::scientific);
	for (int i=0; i <= x_N; i++) {
		pixel_xvals << corner_pts[i][0][0] << endl;
	}
	for (int j=0; j <= y_N; j++) {
		pixel_yvals << corner_pts[0][j][1] << endl;
	}	
	int i,j;
	double residual;
	for (j=0; j < y_N; j++) {
		for (i=0; i < x_N; i++) {
			if ((fit_to_data==NULL) or (fit_to_data[i][j])) {
				if (!plot_residual) pixel_image_file << surface_brightness[i][j];
				else {
					residual = lens->image_pixel_data->surface_brightness[i][j] - surface_brightness[i][j];
					pixel_image_file << residual;
				}
			} else {
				if (!plot_residual) pixel_image_file << "0";
				else pixel_image_file << lens->image_pixel_data->surface_brightness[i][j];
			}
			if (i < x_N-1) pixel_image_file << " ";
		}
		pixel_image_file << endl;
	}
}

void ImagePixelGrid::output_fits_file(string fits_filename, bool plot_residual)
{
#ifndef USE_FITS
	cout << "FITS capability disabled; QLens must be compiled with the CFITSIO library to write FITS files\n"; return;
#else
	int i,j,kk;
	fitsfile *outfptr;   // FITS file pointer, defined in fitsio.h
	int status = 0;   // CFITSIO status value MUST be initialized to zero!
	int bitpix = -64, naxis = 2;
	long naxes[2] = {x_N,y_N};
	double *pixels;
	double x, y, xstep, ystep;

	if (!fits_create_file(&outfptr, fits_filename.c_str(), &status))
	{
		if (!fits_create_img(outfptr, bitpix, naxis, naxes, &status))
		{
			if (naxis == 0) {
				die("Error: only 1D or 2D images are supported (dimension is %i)\n",naxis);
			} else {
				kk=0;
				long fpixel[naxis];
				for (kk=0; kk < naxis; kk++) fpixel[kk] = 1;
				pixels = new double[x_N];

				for (fpixel[1]=1, j=0; fpixel[1] <= naxes[1]; fpixel[1]++, j++)
				{
					for (i=0; i < x_N; i++) {
						if (!plot_residual) pixels[i] = surface_brightness[i][j];
						else pixels[i] = lens->image_pixel_data->surface_brightness[i][j] - surface_brightness[i][j];
					}
					fits_write_pix(outfptr, TDOUBLE, fpixel, naxes[0], pixels, &status);
				}
				delete[] pixels;
			}
		}
		fits_close_file(outfptr, &status);
	} 

	if (status) fits_report_error(stderr, status); // print any error message
#endif
}

void Lens::assign_Lmatrix(bool verbal)
{
	int img_index;
	int index;
	int i,j;
	Lmatrix_rows = new vector<double>[image_npixels];
	Lmatrix_index_rows = new vector<int>[image_npixels];
	int *Lmatrix_row_nn = new int[image_npixels];
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	if (image_pixel_grid->ray_tracing_method == Area_Overlap)
	{
		lensvector *corners[4];
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			#pragma omp for private(img_index,i,j,index,corners) schedule(dynamic)
			for (img_index=0; img_index < image_npixels; img_index++) {
				index=0;
				i = active_image_pixel_i[img_index];
				j = active_image_pixel_j[img_index];
				corners[0] = &image_pixel_grid->corner_sourcepts[i][j];
				corners[1] = &image_pixel_grid->corner_sourcepts[i][j+1];
				corners[2] = &image_pixel_grid->corner_sourcepts[i+1][j];
				corners[3] = &image_pixel_grid->corner_sourcepts[i+1][j+1];
				source_pixel_grid->calculate_Lmatrix_overlap(img_index,i,j,index,corners,thread);
				Lmatrix_row_nn[img_index] = index;
			}
		}
	}
	else if (image_pixel_grid->ray_tracing_method == Interpolate)
	{
		#pragma omp parallel
		{
			int thread;
#ifdef USE_OPENMP
			thread = omp_get_thread_num();
#else
			thread = 0;
#endif
			#pragma omp for private(img_index,i,j,index) schedule(dynamic)
			for (img_index=0; img_index < image_npixels; img_index++) {
				index=0;
				i = active_image_pixel_i[img_index];
				j = active_image_pixel_j[img_index];
				source_pixel_grid->calculate_Lmatrix_interpolate(img_index,i,j,index,image_pixel_grid->center_sourcepts[i][j],thread);
				Lmatrix_row_nn[img_index] = index;
			}
		}
	}

	image_pixel_location_Lmatrix[0] = 0;
	for (img_index=0; img_index < image_npixels; img_index++) {
		image_pixel_location_Lmatrix[img_index+1] = image_pixel_location_Lmatrix[img_index] + Lmatrix_row_nn[img_index];
	}
	if (image_pixel_location_Lmatrix[img_index] != Lmatrix_n_elements) die("Number of Lmatrix elements don't match (%i vs %i)",image_pixel_location_Lmatrix[img_index],Lmatrix_n_elements);

	index=0;
	for (i=0; i < image_npixels; i++) {
		for (j=0; j < Lmatrix_row_nn[i]; j++) {
			Lmatrix[index] = Lmatrix_rows[i][j];
			Lmatrix_index[index] = Lmatrix_index_rows[i][j];
			index++;
		}
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for constructing Lmatrix: " << wtime << endl;
	}
#endif
	if ((mpi_id==0) and (verbal)) {
		int Lmatrix_ntot = source_npixels*image_npixels;
		double sparseness = ((double) Lmatrix_n_elements)/Lmatrix_ntot;
		cout << "image has " << image_pixel_grid->n_active_pixels << " active pixels, Lmatrix has " << Lmatrix_n_elements << " nonzero elements (sparseness " << sparseness << ")\n";
	}

	delete[] Lmatrix_row_nn;
	delete[] Lmatrix_rows;
	delete[] Lmatrix_index_rows;
}

ImagePixelGrid::~ImagePixelGrid()
{
	for (int i=0; i <= x_N; i++) {
		delete[] corner_pts[i];
		delete[] corner_sourcepts[i];
	}
	delete[] corner_pts;
	delete[] corner_sourcepts;
	for (int i=0; i < x_N; i++) {
		delete[] center_pts[i];
		delete[] center_sourcepts[i];
		delete[] center_magnifications[i];
		delete[] maps_to_source_pixel[i];
		delete[] pixel_index[i];
		delete[] mapped_source_pixels[i];
		delete[] surface_brightness[i];
		delete[] source_plane_triangle1_area[i];
		delete[] source_plane_triangle2_area[i];
	}
	delete[] center_pts;
	delete[] center_sourcepts;
	delete[] center_magnifications;
	delete[] maps_to_source_pixel;
	delete[] pixel_index;
	delete[] mapped_source_pixels;
	delete[] surface_brightness;
	delete[] source_plane_triangle1_area;
	delete[] source_plane_triangle2_area;
	if (fit_to_data != NULL) {
		for (int i=0; i < x_N; i++) delete[] fit_to_data[i];
		delete[] fit_to_data;
	}

}

/************************** Functions in class Lens that pertain to pixel mapping and inversion ****************************/

bool Lens::assign_pixel_mappings(bool verbal)
{

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int tot_npixels_count;
	tot_npixels_count = source_pixel_grid->assign_indices_and_count_levels();
	if ((mpi_id==0) and (adaptive_grid) and (verbal==true)) cout << "Number of source cells: " << tot_npixels_count << endl;
	image_pixel_grid->assign_image_mapping_flags();

	//source_pixel_grid->missed_cells_out.open("missed_cells.dat");
	source_pixel_grid->regrid = false;
	source_npixels = source_pixel_grid->assign_active_indices_and_count_source_pixels(regrid_if_unmapped_source_subpixels,activate_unmapped_source_pixels,exclude_source_pixels_beyond_fit_window);
	if (source_npixels==0) { warn(warnings,"number of source pixels cannot be zero"); return false; }
	//source_pixel_grid->missed_cells_out.close();
	while (source_pixel_grid->regrid) {
		if ((mpi_id==0) and (verbal==true)) cout << "Redrawing the source grid after reverse-splitting unmapped source pixels...\n";
		source_pixel_grid->regrid = false;
		source_pixel_grid->assign_all_neighbors();
		tot_npixels_count = source_pixel_grid->assign_indices_and_count_levels();
		if ((mpi_id==0) and (verbal==true)) cout << "Number of source cells after re-gridding: " << tot_npixels_count << endl;
		image_pixel_grid->assign_image_mapping_flags();
		//source_pixel_grid->print_indices();
		source_npixels = source_pixel_grid->assign_active_indices_and_count_source_pixels(regrid_if_unmapped_source_subpixels,activate_unmapped_source_pixels,exclude_source_pixels_beyond_fit_window);
	}

	//image_pixel_grid->plot_center_pts_source_plane();
	image_npixels = image_pixel_grid->n_active_pixels;
	active_image_pixel_i = new int[image_npixels];
	active_image_pixel_j = new int[image_npixels];
	int i, j, image_pixel_index=0;
	for (j=0; j < image_pixel_grid->y_N; j++) {
		for (i=0; i < image_pixel_grid->x_N; i++) {
			if (image_pixel_grid->maps_to_source_pixel[i][j]) {
				active_image_pixel_i[image_pixel_index] = i;
				active_image_pixel_j[image_pixel_index] = j;
				image_pixel_grid->pixel_index[i][j] = image_pixel_index++;
			} else image_pixel_grid->pixel_index[i][j] = -1;
		}
	}
	if (image_pixel_index != image_npixels) die("Number of active pixels (%i) doesn't seem to match image_npixels (%i)",image_pixel_index,image_npixels);

	if ((verbal) and (mpi_id==0)) cout << "source # of pixels: " << source_pixel_grid->number_of_pixels << ", counted up as " << tot_npixels_count << ", # of active pixels: " << source_npixels << endl;
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for assigning pixel mappings: " << wtime << endl;
	}
#endif

	return true;
}

void Lens::initialize_pixel_matrices(bool verbal)
{
	if (Lmatrix != NULL) die("Lmatrix already initialized");
	if (source_surface_brightness != NULL) die("source surface brightness vector already initialized");
	if (image_surface_brightness != NULL) die("image surface brightness vector already initialized");
	image_surface_brightness = new double[image_npixels];
	source_surface_brightness = new double[source_npixels];
	if (n_image_prior) {
		source_pixel_n_images = new double[source_npixels];
		source_pixel_grid->fill_n_image_vector();
	}

	Lmatrix_n_elements = image_pixel_grid->count_nonzero_source_pixel_mappings();
	if ((mpi_id==0) and (verbal)) cout << "Expected Lmatrix_n_elements=" << Lmatrix_n_elements << endl << flush;
	Lmatrix_index = new int[Lmatrix_n_elements];
	image_pixel_location_Lmatrix = new int[image_npixels+1];
	Lmatrix = new double[Lmatrix_n_elements];

	if ((mpi_id==0) and (verbal)) cout << "Creating Lmatrix...\n";
	assign_Lmatrix(verbal);
}

void Lens::clear_pixel_matrices()
{
	if (image_surface_brightness != NULL) delete[] image_surface_brightness;
	if (source_surface_brightness != NULL) delete[] source_surface_brightness;
	if (active_image_pixel_i != NULL) delete[] active_image_pixel_i;
	if (active_image_pixel_j != NULL) delete[] active_image_pixel_j;
	if (image_pixel_location_Lmatrix != NULL) delete[] image_pixel_location_Lmatrix;
	if (Lmatrix_index != NULL) delete[] Lmatrix_index;
	if (Lmatrix != NULL) delete[] Lmatrix;
	if (source_pixel_location_Lmatrix != NULL) delete[] source_pixel_location_Lmatrix;
	image_surface_brightness = NULL;
	source_surface_brightness = NULL;
	active_image_pixel_i = NULL;
	active_image_pixel_j = NULL;
	image_pixel_location_Lmatrix = NULL;
	source_pixel_location_Lmatrix = NULL;
	Lmatrix = NULL;
	Lmatrix_index = NULL;
	for (int i=0; i < image_pixel_grid->x_N; i++) {
		for (int j=0; j < image_pixel_grid->y_N; j++) {
			image_pixel_grid->mapped_source_pixels[i][j].clear();
		}
	}
}

void Lens::PSF_convolution_Lmatrix(bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	if (psf_convolution_mpi) {
		MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
	}
#endif

	if ((mpi_id==0) and (verbal)) cout << "Beginning PSF convolution...\n";
	static const double sigma_fraction = 1.6; // the bigger you make this, the less sparse the matrix will become (more pixel-pixel correlations)
	int nx_half, ny_half, nx, ny;
	double x, y, xmax, ymax;
	int i,j;
	if (use_input_psf_matrix) {
		if (psf_matrix == NULL) return;
		nx = psf_npixels_x;
		ny = psf_npixels_y;
		nx_half = nx/2;
		ny_half = ny/2;
	} else {
		if ((psf_width_x==0) or (psf_width_y==0)) return;
		double normalization = 0;
		double xstep, ystep, nx_half_dec, ny_half_dec;
		xstep = image_pixel_grid->pixel_xlength;
		ystep = image_pixel_grid->pixel_ylength;
		nx_half_dec = sigma_fraction*psf_width_x/xstep;
		ny_half_dec = sigma_fraction*psf_width_y/ystep;
		nx_half = ((int) nx_half_dec);
		ny_half = ((int) ny_half_dec);
		if ((nx_half_dec - nx_half) > 0.5) nx_half++;
		if ((ny_half_dec - ny_half) > 0.5) ny_half++;
		xmax = nx_half*xstep;
		ymax = ny_half*ystep;
		nx = 2*nx_half+1;
		ny = 2*ny_half+1;
		if (psf_matrix != NULL) {
			for (i=0; i < psf_npixels_x; i++) delete[] psf_matrix[i];
			delete[] psf_matrix;
		}
		psf_matrix = new double*[nx];
		for (i=0; i < nx; i++) psf_matrix[i] = new double[ny];
		psf_npixels_x = nx;
		psf_npixels_y = ny;
		for (i=0, x=-xmax; i < nx; i++, x += xstep) {
			for (j=0, y=-ymax; j < ny; j++, y += ystep) {
				psf_matrix[i][j] = exp(-0.5*(SQR(x/psf_width_x) + SQR(y/psf_width_y)));
				normalization += psf_matrix[i][j];
			}
		}
		for (i=0; i < nx; i++) {
			for (j=0; j < ny; j++) {
				psf_matrix[i][j] /= normalization;
			}
		}
	}
	int *Lmatrix_psf_row_nn = new int[image_npixels];
	vector<double> *Lmatrix_psf_rows = new vector<double>[image_npixels];
	vector<int> *Lmatrix_psf_index_rows = new vector<int>[image_npixels];

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	// If the PSF is sufficiently wide, it may save time to MPI the PSF convolution by setting psf_convolution_mpi to 'true'. This option is off by default.
	int mpi_chunk, mpi_start, mpi_end;
	if (psf_convolution_mpi) {
		mpi_chunk = image_npixels / group_np;
		mpi_start = group_id*mpi_chunk;
		if (group_id == group_np-1) mpi_chunk += (image_npixels % group_np); // assign the remainder elements to the last mpi process
		mpi_end = mpi_start + mpi_chunk;
	} else {
		mpi_start = 0; mpi_end = image_npixels;
	}

	int k,l,m;
	int psf_k, psf_l;
	int img_index1, img_index2, src_index, col_index;
	int index;
	bool new_entry;
	int Lmatrix_psf_nn=0;
	int Lmatrix_psf_nn_part=0;
	int src_index1, src_index2;
	#pragma omp parallel for private(m,k,l,i,j,img_index1,img_index2,src_index,src_index1,src_index2,col_index,psf_k,psf_l,index,new_entry) schedule(static) reduction(+:Lmatrix_psf_nn_part)
	for (img_index1=mpi_start; img_index1 < mpi_end; img_index1++)
	{ // this loops over columns of the PSF blurring matrix
		int col_i=0;
		Lmatrix_psf_row_nn[img_index1] = 0;
		k = active_image_pixel_i[img_index1];
		l = active_image_pixel_j[img_index1];
		for (psf_k=0; psf_k < ny; psf_k++) {
			i = k + ny_half - psf_k;
			if ((i >= 0) and (i < image_pixel_grid->x_N)) {
				for (psf_l=0; psf_l < nx; psf_l++) {
					j = l + nx_half - psf_l;
					if ((j >= 0) and (j < image_pixel_grid->y_N)) {
						if (image_pixel_grid->maps_to_source_pixel[i][j]) {
							img_index2 = image_pixel_grid->pixel_index[i][j];

							for (index=image_pixel_location_Lmatrix[img_index2]; index < image_pixel_location_Lmatrix[img_index2+1]; index++) {
								src_index = Lmatrix_index[index];
								new_entry = true;
								for (m=0; m < Lmatrix_psf_row_nn[img_index1]; m++) {
									if (Lmatrix_psf_index_rows[img_index1][m]==src_index) { col_index=m; new_entry=false; }
								}
								if (new_entry) {
									Lmatrix_psf_rows[img_index1].push_back(psf_matrix[psf_l][psf_k]*Lmatrix[index]);
									Lmatrix_psf_index_rows[img_index1].push_back(src_index);
									Lmatrix_psf_row_nn[img_index1]++;
									col_i++;
								} else {
									Lmatrix_psf_rows[img_index1][col_index] += psf_matrix[psf_l][psf_k]*Lmatrix[index];
								}
							}
						}
					}
				}
			}
		}
		Lmatrix_psf_nn_part += col_i;
	}

#ifdef USE_MPI
	if (psf_convolution_mpi)
		MPI_Allreduce(&Lmatrix_psf_nn_part, &Lmatrix_psf_nn, 1, MPI_INT, MPI_SUM, sub_comm);
	else
		Lmatrix_psf_nn = Lmatrix_psf_nn_part;
#else
	Lmatrix_psf_nn = Lmatrix_psf_nn_part;
#endif

	double *Lmatrix_psf = new double[Lmatrix_psf_nn];
	int *Lmatrix_index_psf = new int[Lmatrix_psf_nn];
	int *image_pixel_location_Lmatrix_psf = new int[image_npixels+1];

#ifdef USE_MPI
	if (psf_convolution_mpi) {
		int id, chunk, start, end, length;
		for (id=0; id < group_np; id++) {
			chunk = image_npixels / group_np;
			start = id*chunk;
			if (id == group_np-1) chunk += (image_npixels % group_np); // assign the remainder elements to the last mpi process
			MPI_Bcast(Lmatrix_psf_row_nn + start,chunk,MPI_INT,id,sub_comm);
		}
	}
#endif

	image_pixel_location_Lmatrix_psf[0] = 0;
	for (m=0; m < image_npixels; m++) {
		image_pixel_location_Lmatrix_psf[m+1] = image_pixel_location_Lmatrix_psf[m] + Lmatrix_psf_row_nn[m];
	}

	int indx;
	for (m=mpi_start; m < mpi_end; m++) {
		indx = image_pixel_location_Lmatrix_psf[m];
		for (j=0; j < Lmatrix_psf_row_nn[m]; j++) {
			Lmatrix_psf[indx+j] = Lmatrix_psf_rows[m][j];
			Lmatrix_index_psf[indx+j] = Lmatrix_psf_index_rows[m][j];
		}
	}

#ifdef USE_MPI
	if (psf_convolution_mpi) {
		int id, chunk, start, end, length;
		for (id=0; id < group_np; id++) {
			chunk = image_npixels / group_np;
			start = id*chunk;
			if (id == group_np-1) chunk += (image_npixels % group_np); // assign the remainder elements to the last mpi process
			end = start + chunk;
			length = image_pixel_location_Lmatrix_psf[end] - image_pixel_location_Lmatrix_psf[start];
			MPI_Bcast(Lmatrix_psf + image_pixel_location_Lmatrix_psf[start],length,MPI_DOUBLE,id,sub_comm);
			MPI_Bcast(Lmatrix_index_psf + image_pixel_location_Lmatrix_psf[start],length,MPI_INT,id,sub_comm);
		}
		MPI_Comm_free(&sub_comm);
	}
#endif

	delete[] Lmatrix_psf_row_nn;

	if ((mpi_id==0) and (verbal)) cout << "Lmatrix after PSF convolution: Lmatrix now has " << indx << " nonzero elements\n";

	delete[] Lmatrix;
	delete[] Lmatrix_index;
	delete[] image_pixel_location_Lmatrix;
	Lmatrix = Lmatrix_psf;
	Lmatrix_index = Lmatrix_index_psf;
	image_pixel_location_Lmatrix = image_pixel_location_Lmatrix_psf;
	Lmatrix_n_elements = Lmatrix_psf_nn;

	delete[] Lmatrix_psf_rows;
	delete[] Lmatrix_psf_index_rows;

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating PSF-convolution of Lmatrix: " << wtime << endl;
	}
#endif
}

void Lens::generate_Rmatrix_from_image_plane_curvature()
{
	cout << "Generating Rmatrix from image plane curvature...\n";
	int i,j,k,l,m,n,indx;

	double curvature_submatrix[3][3] = {{0,1,0},{1,-4,1},{0,1,0}};

	int *curvature_matrix_row_nn = new int[image_npixels];
	vector<double> *curvature_matrix_rows = new vector<double>[image_npixels];
	vector<int> *curvature_matrix_index_rows = new vector<int>[image_npixels];

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif

	int curv_k, curv_l;
	int img_index1, img_index2, src_index, col_index;
	int index;
	bool new_entry;
	int curvature_matrix_nn=0;
	int curvature_matrix_nn_part=0;
	#pragma omp parallel for private(m,k,l,i,j,img_index1,img_index2,src_index,col_index,curv_k,curv_l,index,new_entry) schedule(static) reduction(+:curvature_matrix_nn_part)
	for (img_index1=0; img_index1 < image_npixels; img_index1++) {
		int col_i=0;
		curvature_matrix_row_nn[img_index1] = 0;
		k = active_image_pixel_i[img_index1];
		l = active_image_pixel_j[img_index1];
		for (curv_k=0; curv_k < 3; curv_k++) {
			i = k + 1 - curv_k;
			if ((i >= 0) and (i < image_pixel_grid->x_N)) {
				for (curv_l=0; curv_l < 3; curv_l++) {
					j = l + 1 - curv_l;
					if ((j >= 0) and (j < image_pixel_grid->y_N)) {
						if (image_pixel_grid->maps_to_source_pixel[i][j]) {
							img_index2 = image_pixel_grid->pixel_index[i][j];

							for (index=image_pixel_location_Lmatrix[img_index2]; index < image_pixel_location_Lmatrix[img_index2+1]; index++) {
								src_index = Lmatrix_index[index];
								//if (src_index==4841) cout << "Lmatrix element: " << Lmatrix[index] << endl;
								if (curvature_submatrix[curv_l][curv_k] != 0) {
									new_entry = true;
									for (m=0; m < curvature_matrix_row_nn[img_index1]; m++) {
										if (curvature_matrix_index_rows[img_index1][m]==src_index) { col_index=m; new_entry=false; }
									}
									if (new_entry) {
										curvature_matrix_rows[img_index1].push_back(curvature_submatrix[curv_l][curv_k]*Lmatrix[index]);
										curvature_matrix_index_rows[img_index1].push_back(src_index);
										curvature_matrix_row_nn[img_index1]++;
										col_i++;
									} else {
										curvature_matrix_rows[img_index1][col_index] += curvature_submatrix[curv_l][curv_k]*Lmatrix[index];
									}
								}
							}
						}
					}
				}
			}
		}
		curvature_matrix_nn_part += col_i;
	}

	curvature_matrix_nn = curvature_matrix_nn_part;

	double *curvature_matrix;
	int *curvature_index;
	int *curvature_row_index;
	curvature_matrix = new double[curvature_matrix_nn];
	curvature_index = new int[curvature_matrix_nn];
	curvature_row_index = new int[image_npixels+1];

	curvature_row_index[0] = 0;
	for (m=0; m < image_npixels; m++) {
		curvature_row_index[m+1] = curvature_row_index[m] + curvature_matrix_row_nn[m];
	}

	for (m=0; m < image_npixels; m++) {
		indx = curvature_row_index[m];
		for (j=0; j < curvature_matrix_row_nn[m]; j++) {
			curvature_matrix[indx+j] = curvature_matrix_rows[m][j];
			curvature_index[indx+j] = curvature_matrix_index_rows[m][j];
		}
	}

	/*
	double curvsum=0;
	dvector ivec(image_npixels);
	dvector jvec(image_npixels);
	int index;
	for (j=0; j < image_pixel_grid->y_N; j++) {
		for (i=0; i < image_pixel_grid->x_N; i++) {
			if (image_pixel_grid->maps_to_source_pixel[i][j]) {
				index = image_pixel_grid->pixel_index[i][j];
				ivec[index] = -2*image_pixel_grid->surface_brightness[i][j];
				jvec[index] = -2*image_pixel_grid->surface_brightness[i][j];
				if ((i>0) and (image_pixel_grid->maps_to_source_pixel[i-1][j])) ivec[index] += image_pixel_grid->surface_brightness[i-1][j];
				if ((i<image_pixel_grid->x_N-1) and (image_pixel_grid->maps_to_source_pixel[i+1][j])) ivec[index] += image_pixel_grid->surface_brightness[i+1][j];
				if ((j>0) and (image_pixel_grid->maps_to_source_pixel[i][j-1])) ivec[index] += image_pixel_grid->surface_brightness[i][j-1];
				if ((j<image_pixel_grid->y_N-1) and (image_pixel_grid->maps_to_source_pixel[i][j+1])) ivec[index] += image_pixel_grid->surface_brightness[i][j+1];
			}
		}
	}
	for (i=0; i < image_npixels; i++) {
		curvsum_x += ivec[i]*ivec[i];
		curvsum_y += jvec[i]*jvec[i];
	}

	dvector ivec2(image_npixels);
	for (i=0; i < image_npixels; i++) {
		ivec2[i] = 0;
		for (j=curvature_row_index[0][i]; j < curvature_row_index[0][i+1]; j++) {
			ivec2[i] += curvature_matrix[0][j]*source_surface_brightness[curvature_index[0][j]];
		}
		jvec2[i] = 0;
		for (j=curvature_row_index[1][i]; j < curvature_row_index[1][i+1]; j++) {
			jvec2[i] += curvature_matrix[1][j]*source_surface_brightness[curvature_index[1][j]];
		}

	}
	double curvsum2_x=0;
	double curvsum2_y=0;
	//for (i=0; i < image_npixels; i++) cout << ivec[i] << " " << ivec2[i] << endl;
	for (i=0; i < image_npixels; i++) curvsum2_x += ivec2[i]*ivec2[i];
	for (i=0; i < image_npixels; i++) curvsum2_y += jvec2[i]*jvec2[i];
	//cout << curvsum << " " << curvsum2 << endl;
	die();
	*/

	/*
	ofstream curv_out("xcurv.dat");
	for (m=mpi_id; m < image_npixels; m += mpi_np) {
		for (j=0; j < xcurvature_matrix_row_nn[m]; j++) {
			curv_out << xcurvature_matrix_index_rows[m][j] << " " << m << ": xcurv= " << xcurvature_matrix_rows[m][j] << endl;
		}
	}
	curv_out.close();
	*/

	delete[] curvature_matrix_row_nn;

	vector<int> *jvals = new vector<int>[source_npixels];
	vector<int> *lvals = new vector<int>[source_npixels];

	Rmatrix_diags = new double[source_npixels];
	Rmatrix_rows = new vector<double>[source_npixels];
	Rmatrix_index_rows = new vector<int>[source_npixels];
	Rmatrix_row_nn = new int[source_npixels];
	Rmatrix_nn = 0;
	int Rmatrix_nn_part = 0;
	for (j=0; j < source_npixels; j++) {
		Rmatrix_diags[j] = 0;
		Rmatrix_row_nn[j] = 0;
	}

	int src_index1, src_index2, col_i;
	double tmp, element;
	int tmp_i;

	for (i=0; i < image_npixels; i++) {
		for (j=curvature_row_index[i]; j < curvature_row_index[i+1]; j++) {
			for (l=j; l < curvature_row_index[i+1]; l++) {
				src_index1 = curvature_index[j];
				src_index2 = curvature_index[l];
				if (src_index1 > src_index2) {
					tmp=src_index1;
					src_index1=src_index2;
					src_index2=tmp;
					jvals[src_index1].push_back(l);
					lvals[src_index1].push_back(j);
				} else {
					jvals[src_index1].push_back(j);
					lvals[src_index1].push_back(l);
				}
			}
		}
	}

	#pragma omp parallel for private(i,j,k,l,m,n,src_index1,src_index2,new_entry,col_index,col_i,element) schedule(static) reduction(+:Rmatrix_nn_part)
	for (src_index1=0; src_index1 < source_npixels; src_index1++) {
		col_i=0;
		for (n=0; n < jvals[src_index1].size(); n++) {
			j = jvals[src_index1][n];
			l = lvals[src_index1][n];
			src_index2 = curvature_index[l];
			new_entry = true;
			element = curvature_matrix[j]*curvature_matrix[l]; // generalize this to full covariance matrix later
			if (src_index1==src_index2) Rmatrix_diags[src_index1] += element;
			else {
				m=0;
				while ((m < Rmatrix_row_nn[src_index1]) and (new_entry==true)) {
					if (Rmatrix_index_rows[src_index1][m]==src_index2) {
						new_entry = false;
						col_index = m;
					}
					m++;
				}
				if (new_entry) {
					Rmatrix_rows[src_index1].push_back(element);
					Rmatrix_index_rows[src_index1].push_back(src_index2);
					Rmatrix_row_nn[src_index1]++;
					col_i++;
				}
				else Rmatrix_rows[src_index1][col_index] += element;
			}
		}
		Rmatrix_nn_part += col_i;
	}

	delete[] curvature_matrix;
	delete[] curvature_index;
	delete[] curvature_row_index;

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating Rmatrix: " << wtime << endl;
	}
#endif

	Rmatrix_nn = Rmatrix_nn_part;
	Rmatrix_nn += source_npixels+1;

	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];

	for (i=0; i < source_npixels; i++)
		Rmatrix[i] = Rmatrix_diags[i];

	Rmatrix_index[0] = source_npixels+1;
	for (i=0; i < source_npixels; i++) {
		Rmatrix_index[i+1] = Rmatrix_index[i] + Rmatrix_row_nn[i];
	}

	for (i=mpi_id; i < source_npixels; i += mpi_np) {
		indx = Rmatrix_index[i];
		for (j=0; j < Rmatrix_row_nn[i]; j++) {
			Rmatrix[indx+j] = Rmatrix_rows[i][j];
			Rmatrix_index[indx+j] = Rmatrix_index_rows[i][j];
		}
	}

	delete[] Rmatrix_row_nn;
	delete[] Rmatrix_diags;
	delete[] Rmatrix_rows;
	delete[] Rmatrix_index_rows;

	delete[] curvature_matrix_row_nn;
	delete[] curvature_matrix_rows;
	delete[] curvature_matrix_index_rows;

	delete[] jvals;
	delete[] lvals;

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating image plane curvature matrices: " << wtime << endl;
	}
#endif
}

void Lens::generate_Rmatrix_norm()
{
	int i,j;
	Rmatrix_diags = new double[source_npixels];
	Rmatrix_rows = new vector<double>[source_npixels];
	Rmatrix_index_rows = new vector<int>[source_npixels];
	Rmatrix_row_nn = new int[source_npixels];

	Rmatrix_nn = source_npixels+1;
	for (i=0; i < source_npixels; i++) {
		Rmatrix_row_nn[i] = 0;
		Rmatrix_diags[i] = 1;
	}

	Rmatrix = new double[Rmatrix_nn];
	Rmatrix_index = new int[Rmatrix_nn];
	for (i=0; i < source_npixels; i++) Rmatrix[i] = Rmatrix_diags[i];

	Rmatrix_index[0] = source_npixels+1;
	for (i=0; i < source_npixels; i++)
		Rmatrix_index[i+1] = Rmatrix_index[i] + Rmatrix_row_nn[i];

	int indx;
	for (i=0; i < source_npixels; i++) {
		indx = Rmatrix_index[i];
		for (j=0; j < Rmatrix_row_nn[i]; j++) {
			Rmatrix[indx+j] = Rmatrix_rows[i][j];
			Rmatrix_index[indx+j] = Rmatrix_index_rows[i][j];
			if (Rmatrix_index[indx+j] <= i) die("off-diagonal indices should be greater than i! %i vs %i",i,Rmatrix_index[indx+j]);
		}
	}
	
	delete[] Rmatrix_row_nn;
	delete[] Rmatrix_diags;
	delete[] Rmatrix_rows;
	delete[] Rmatrix_index_rows;
}

void Lens::create_regularization_matrix()
{
	if (Rmatrix != NULL) delete[] Rmatrix;
	if (Rmatrix_index != NULL) delete[] Rmatrix_index;

	int i,j;

	switch (regularization_method) {
		case Norm:
			generate_Rmatrix_norm(); break;
		case Gradient:
			generate_Rmatrix_from_gmatrices(); break;
		case Curvature:
			generate_Rmatrix_from_hmatrices(); break;
		case Image_Plane_Curvature:
			generate_Rmatrix_from_image_plane_curvature(); break;
		default:
			die("Regularization method not recognized");
	}
}

void Lens::create_lensing_matrices_from_Lmatrix(bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
#endif

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	double effective_reg_parameter = regularization_parameter;

	double covariance; // right now we're using a uniform uncorrelated noise for each pixel; will generalize this later
	if (data_pixel_noise==0) covariance = 1; // if there is no noise it doesn't matter what the covariance is, since we won't be regularizing
	else covariance = SQR(data_pixel_noise);

	int i,j,k,l,m,t;

	vector<jl_pair> **jlvals = new vector<jl_pair>*[nthreads];
	for (i=0; i < nthreads; i++) {
		jlvals[i] = new vector<jl_pair>[source_npixels];
	}

	vector<int> *Fmatrix_index_rows = new vector<int>[source_npixels];
	vector<double> *Fmatrix_rows = new vector<double>[source_npixels];
	double *Fmatrix_diags = new double[source_npixels];
	int *Fmatrix_row_nn = new int[source_npixels];
	int Fmatrix_nn = 0;
	int Fmatrix_nn_part = 0;
	for (j=0; j < source_npixels; j++) {
		Fmatrix_diags[j] = 0;
		Fmatrix_row_nn[j] = 0;
	}

	bool new_entry;
	int src_index1, src_index2, col_index, col_i;
	double tmp, element;
	int tmp_i;
	Dvector = new double[source_npixels];
	for (i=0; i < source_npixels; i++) Dvector[i] = 0;

	for (i=0; i < image_npixels; i++) {
		for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
			Dvector[Lmatrix_index[j]] += Lmatrix[j]*image_surface_brightness[i]/covariance;
		}
	}

	int mpi_chunk, mpi_start, mpi_end;
	mpi_chunk = source_npixels / group_np;
	mpi_start = group_id*mpi_chunk;
	if (group_id == group_np-1) mpi_chunk += (source_npixels % group_np); // assign the remainder elements to the last mpi process
	mpi_end = mpi_start + mpi_chunk;

	jl_pair jl;

	#pragma omp parallel
	{
		int thread;
#ifdef USE_OPENMP
		thread = omp_get_thread_num();
#else
		thread = 0;
#endif
	// idea: just store j and l, so that all the calculating can be done in the loop below (which can be made parallel much more easily)
		#pragma omp for private(i,j,l,jl,src_index1,src_index2,tmp) schedule(dynamic)
		for (i=0; i < image_npixels; i++) {
			for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
				for (l=j; l < image_pixel_location_Lmatrix[i+1]; l++) {
					src_index1 = Lmatrix_index[j];
					src_index2 = Lmatrix_index[l];
					if (src_index1 > src_index2) {
						jl.l=j; jl.j=l;
						jlvals[thread][src_index2].push_back(jl);
					} else {
						jl.j=j; jl.l=l;
						jlvals[thread][src_index1].push_back(jl);
					}
				}
			}
		}

#ifdef USE_OPENMP
		#pragma omp barrier
		#pragma omp master
		{
			if (show_wtime) {
				wtime = omp_get_wtime() - wtime0;
				if (mpi_id==0) cout << "Wall time for calculating Fmatrix (storing jvals,lvals): " << wtime << endl;
				wtime0 = omp_get_wtime();
			}
		}
#endif

		#pragma omp for private(i,j,k,l,m,t,src_index1,src_index2,new_entry,col_index,col_i,element) schedule(static) reduction(+:Fmatrix_nn_part)
		for (src_index1=mpi_start; src_index1 < mpi_end; src_index1++) {
			col_i=0;
			for (t=0; t < nthreads; t++) {
				for (k=0; k < jlvals[t][src_index1].size(); k++) {
					j = jlvals[t][src_index1][k].j;
					l = jlvals[t][src_index1][k].l;
					src_index2 = Lmatrix_index[l];
					new_entry = true;
					element = Lmatrix[j]*Lmatrix[l]/covariance; // generalize this to full covariance matrix later
					if (src_index1==src_index2) Fmatrix_diags[src_index1] += element;
					else {
						m=0;
						while ((m < Fmatrix_row_nn[src_index1]) and (new_entry==true))
						{
							if (Fmatrix_index_rows[src_index1][m]==src_index2) {
								new_entry = false;
								col_index = m;
							}
							m++;
						}
						if (new_entry) {
							Fmatrix_rows[src_index1].push_back(element);
							Fmatrix_index_rows[src_index1].push_back(src_index2);
							Fmatrix_row_nn[src_index1]++;
							col_i++;
						}
						else Fmatrix_rows[src_index1][col_index] += element;
					}
				}
			}
			Fmatrix_nn_part += col_i;

			if (regularization_method != None) {
				Fmatrix_diags[src_index1] += effective_reg_parameter*Rmatrix[src_index1];
				col_i=0;
				for (j=Rmatrix_index[src_index1]; j < Rmatrix_index[src_index1+1]; j++) {
					new_entry = true;
					k=0;
					while ((k < Fmatrix_row_nn[src_index1]) and (new_entry==true)) {
						if (Rmatrix_index[j]==Fmatrix_index_rows[src_index1][k]) {
							new_entry = false;
							col_index = k;
						}
						k++;
					}
					if (new_entry) {
						Fmatrix_rows[src_index1].push_back(effective_reg_parameter*Rmatrix[j]);
						Fmatrix_index_rows[src_index1].push_back(Rmatrix_index[j]);
						Fmatrix_row_nn[src_index1]++;
						col_i++;
					} else {
						Fmatrix_rows[src_index1][col_index] += effective_reg_parameter*Rmatrix[j];
					}
				}
				Fmatrix_nn_part += col_i;
			}
		}
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for calculating Fmatrix elements: " << wtime << endl;
		wtime0 = omp_get_wtime();
	}
#endif

#ifdef USE_MPI
	MPI_Allreduce(&Fmatrix_nn_part, &Fmatrix_nn, 1, MPI_INT, MPI_SUM, sub_comm);
#else
	Fmatrix_nn = Fmatrix_nn_part;
#endif
	Fmatrix_nn += source_npixels+1;

	Fmatrix = new double[Fmatrix_nn];
	Fmatrix_index = new int[Fmatrix_nn];

#ifdef USE_MPI
	int id, chunk, start, end, length;
	for (id=0; id < group_np; id++) {
		chunk = source_npixels / group_np;
		start = id*chunk;
		if (id == group_np-1) chunk += (source_npixels % group_np); // assign the remainder elements to the last mpi process
		MPI_Bcast(Fmatrix_row_nn + start,chunk,MPI_INT,id,sub_comm);
		MPI_Bcast(Fmatrix_diags + start,chunk,MPI_DOUBLE,id,sub_comm);
	}
#endif

	Fmatrix_index[0] = source_npixels+1;
	for (i=0; i < source_npixels; i++) {
		Fmatrix_index[i+1] = Fmatrix_index[i] + Fmatrix_row_nn[i];
	}
	if (Fmatrix_index[source_npixels] != Fmatrix_nn) die("Fmatrix # of elements don't match up (%i vs %i), process %i",Fmatrix_index[source_npixels],Fmatrix_nn,mpi_id);

	for (i=0; i < source_npixels; i++)
		Fmatrix[i] = Fmatrix_diags[i];

	int indx;
	for (i=mpi_start; i < mpi_end; i++) {
		indx = Fmatrix_index[i];
		for (j=0; j < Fmatrix_row_nn[i]; j++) {
			Fmatrix[indx+j] = Fmatrix_rows[i][j];
			Fmatrix_index[indx+j] = Fmatrix_index_rows[i][j];
		}
	}

#ifdef USE_MPI
	for (id=0; id < group_np; id++) {
		chunk = source_npixels / group_np;
		start = id*chunk;
		if (id == group_np-1) chunk += (source_npixels % group_np); // assign the remainder elements to the last mpi process
		end = start + chunk;
		length = Fmatrix_index[end] - Fmatrix_index[start];
		MPI_Bcast(Fmatrix + Fmatrix_index[start],length,MPI_DOUBLE,id,sub_comm);
		MPI_Bcast(Fmatrix_index + Fmatrix_index[start],length,MPI_INT,id,sub_comm);
	}
	MPI_Comm_free(&sub_comm);
#endif

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for Fmatrix MPI communication + construction: " << wtime << endl;
	}
#endif

	if ((mpi_id==0) and (verbal)) cout << "Fmatrix now has " << Fmatrix_nn << " elements\n";

	if ((mpi_id==0) and (verbal)) {
		int Fmatrix_ntot = source_npixels*(source_npixels+1)/2;
		double sparseness = ((double) Fmatrix_nn)/Fmatrix_ntot;
		cout << "Fmatrix sparseness = " << sparseness << endl;
	}

	for (i=0; i < nthreads; i++) {
		delete[] jlvals[i];
	}
	delete[] jlvals;
	delete[] Fmatrix_index_rows;
	delete[] Fmatrix_rows;
	delete[] Fmatrix_diags;
	delete[] Fmatrix_row_nn;
}

void Lens::invert_lens_mapping_CG_method(bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
#endif

#ifdef USE_MPI
	MPI_Barrier(sub_comm);
#endif

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int i,k;
	double *temp = new double[source_npixels];
	// it would be prettier to just pass the MPI communicator in, and have CG_sparse figure out the rank and # of processes internally--implement this later
	CG_sparse cg_method(Fmatrix,Fmatrix_index,1e-4,100000,inversion_nthreads,group_np,group_id);
#ifdef USE_MPI
	cg_method.set_MPI_comm(&sub_comm);
#endif
	for (int i=0; i < source_npixels; i++) temp[i] = 0;
	if ((regularization_method != None) and ((vary_regularization_parameter) or (vary_pixel_fraction)))
		cg_method.set_determinant_mode(true);
	else cg_method.set_determinant_mode(false);
#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for setting up CG method: " << wtime << endl;
		wtime0 = omp_get_wtime();
	}
#endif
	cg_method.solve(Dvector,temp);

	if ((n_image_prior) or (max_sb_prior_unselected_pixels)) {
		max_pixel_sb=-1e30;
		int max_sb_i;
		for (int i=0; i < source_npixels; i++) {
			if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_surface_brightness[i] = temp[i];
			if (source_surface_brightness[i] > max_pixel_sb) {
				max_pixel_sb = source_surface_brightness[i];
				max_sb_i = i;
			}
		}
		if (n_image_prior) n_images_at_sbmax = source_pixel_n_images[max_sb_i];
	} else {
		for (int i=0; i < source_npixels; i++) {
			if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_surface_brightness[i] = temp[i];
		}
	}

	if ((regularization_method != None) and ((vary_regularization_parameter) or (vary_pixel_fraction))) {
		cg_method.get_log_determinant(Fmatrix_log_determinant);
		if ((mpi_id==0) and (verbal)) cout << "log determinant = " << Fmatrix_log_determinant << endl;
		CG_sparse cg_det(Rmatrix,Rmatrix_index,3e-4,100000,inversion_nthreads,group_np,group_id);
#ifdef USE_MPI
		cg_det.set_MPI_comm(&sub_comm);
#endif
		Rmatrix_log_determinant = cg_det.calculate_log_determinant();
		if ((mpi_id==0) and (verbal)) cout << "Rmatrix log determinant = " << Rmatrix_log_determinant << endl;
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for inverting Fmatrix: " << wtime << endl;
	}
#endif

	int iterations;
	double error;
	cg_method.get_error(iterations,error);
	if ((mpi_id==0) and (verbal)) cout << iterations << " iterations, error=" << error << endl << endl;

	delete[] temp;
	int index=0;
	source_pixel_grid->update_surface_brightness(index);
#ifdef USE_MPI
	MPI_Comm_free(&sub_comm);
#endif
}

void Lens::invert_lens_mapping_UMFPACK(bool verbal)
{
#ifndef USE_UMFPACK
	die("QLens requires compilation with UMFPACK for factorization");
#else
	bool calculate_determinant = false;
	int default_nthreads=1;

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int i,j;

   double *null = (double *) NULL ;
	double *temp = new double[source_npixels];
   void *Symbolic, *Numeric ;
	double Control [UMFPACK_CONTROL];
	double Info [UMFPACK_INFO];
    umfpack_di_defaults (Control) ;
	 Control[UMFPACK_STRATEGY] = UMFPACK_STRATEGY_SYMMETRIC;

	int Fmatrix_nonzero_elements = Fmatrix_index[source_npixels]-1;
	int Fmatrix_offdiags = Fmatrix_index[source_npixels]-1-source_npixels;
	int Fmatrix_unsymmetric_nonzero_elements = source_npixels + 2*Fmatrix_offdiags;
	if (Fmatrix_nonzero_elements==0) {
		cout << "nsource_pixels=" << source_npixels << endl;
		die("Fmatrix has zero size");
	}

	// Now we construct the transpose of Fmatrix so we can cast it into "unsymmetric" format for UMFPACK (by including offdiagonals on either side of diagonal elements)
	double *Fmatrix_transpose = new double[Fmatrix_nonzero_elements+1];
	int *Fmatrix_transpose_index = new int[Fmatrix_nonzero_elements+1];

	int k,jl,jm,jp,ju,m,n2,noff,inc,iv;
	double v;

	n2=Fmatrix_index[0];
	for (j=0; j < n2-1; j++) Fmatrix_transpose[j] = Fmatrix[j];
	int n_offdiag = Fmatrix_index[n2-1] - Fmatrix_index[0];
	int *offdiag_indx = new int[n_offdiag];
	int *offdiag_indx_transpose = new int[n_offdiag];
	for (i=0; i < n_offdiag; i++) offdiag_indx[i] = Fmatrix_index[n2+i];
	indexx(offdiag_indx,offdiag_indx_transpose,n_offdiag);
	for (j=n2, k=0; j < Fmatrix_index[n2-1]; j++, k++) {
		Fmatrix_transpose_index[j] = offdiag_indx_transpose[k];
	}
	jp=0;
	for (k=Fmatrix_index[0]; k < Fmatrix_index[n2-1]; k++) {
		m = Fmatrix_transpose_index[k] + n2;
		Fmatrix_transpose[k] = Fmatrix[m];
		for (j=jp; j < Fmatrix_index[m]+1; j++)
			Fmatrix_transpose_index[j]=k;
		jp = Fmatrix_index[m] + 1;
		jl=0;
		ju=n2-1;
		while (ju-jl > 1) {
			jm = (ju+jl)/2;
			if (Fmatrix_index[jm] > m) ju=jm; else jl=jm;
		}
		Fmatrix_transpose_index[k]=jl;
	}
	for (j=jp; j < n2; j++) Fmatrix_transpose_index[j] = Fmatrix_index[n2-1];
	for (j=0; j < n2-1; j++) {
		jl = Fmatrix_transpose_index[j+1] - Fmatrix_transpose_index[j];
		noff=Fmatrix_transpose_index[j];
		inc=1;
		do {
			inc *= 3;
			inc++;
		} while (inc <= jl);
		do {
			inc /= 3;
			for (k=noff+inc; k < noff+jl; k++) {
				iv = Fmatrix_transpose_index[k];
				v = Fmatrix_transpose[k];
				m=k;
				while (Fmatrix_transpose_index[m-inc] > iv) {
					Fmatrix_transpose_index[m] = Fmatrix_transpose_index[m-inc];
					Fmatrix_transpose[m] = Fmatrix_transpose[m-inc];
					m -= inc;
					if (m-noff+1 <= inc) break;
				}
				Fmatrix_transpose_index[m] = iv;
				Fmatrix_transpose[m] = v;
			}
		} while (inc > 1);
	}
	delete[] offdiag_indx;
	delete[] offdiag_indx_transpose;

	int *Fmatrix_unsymmetric_cols = new int[source_npixels+1];
	int *Fmatrix_unsymmetric_indices = new int[Fmatrix_unsymmetric_nonzero_elements];
	double *Fmatrix_unsymmetric = new double[Fmatrix_unsymmetric_nonzero_elements];

	int indx=0;
	Fmatrix_unsymmetric_cols[0] = 0;
	for (i=0; i < source_npixels; i++) {
		for (j=Fmatrix_transpose_index[i]; j < Fmatrix_transpose_index[i+1]; j++) {
			Fmatrix_unsymmetric[indx] = Fmatrix_transpose[j];
			Fmatrix_unsymmetric_indices[indx] = Fmatrix_transpose_index[j];
			indx++;
		}
		Fmatrix_unsymmetric_indices[indx] = i;
		Fmatrix_unsymmetric[indx] = Fmatrix[i];
		indx++;
		for (j=Fmatrix_index[i]; j < Fmatrix_index[i+1]; j++) {
			Fmatrix_unsymmetric[indx] = Fmatrix[j];
			Fmatrix_unsymmetric_indices[indx] = Fmatrix_index[j];
			indx++;
		}
		Fmatrix_unsymmetric_cols[i+1] = indx;
	}

	//cout << "Dvector: " << endl;
	//for (i=0; i < source_npixels; i++) {
		//cout << Dvector[i] << " ";
	//}
	//cout << endl;

	for (i=0; i < source_npixels; i++) {
		sort(Fmatrix_unsymmetric_cols[i+1]-Fmatrix_unsymmetric_cols[i],Fmatrix_unsymmetric_indices+Fmatrix_unsymmetric_cols[i],Fmatrix_unsymmetric+Fmatrix_unsymmetric_cols[i]);
		//cout << "Row " << i << ": " << endl;
		//cout << Fmatrix_unsymmetric_cols[i] << " ";
		//for (j=Fmatrix_unsymmetric_cols[i]; j < Fmatrix_unsymmetric_cols[i+1]; j++) {
			//cout << Fmatrix_unsymmetric_indices[j] << " ";
		//}
		//cout << endl;
		//for (j=Fmatrix_unsymmetric_cols[i]; j < Fmatrix_unsymmetric_cols[i+1]; j++) {
			//cout << "j=" << j << " " << Fmatrix_unsymmetric[j] << " ";
		//}
		//cout << endl;
	}
	//cout << endl;

	if (indx != Fmatrix_unsymmetric_nonzero_elements) die("WTF! Wrong number of nonzero elements");

	int status;
   status = umfpack_di_symbolic(source_npixels, source_npixels, Fmatrix_unsymmetric_cols, Fmatrix_unsymmetric_indices, Fmatrix_unsymmetric, &Symbolic, Control, Info);
	if (status < 0) {
		umfpack_di_report_info (Control, Info) ;
		umfpack_di_report_status (Control, status) ;
		die("Error inputting matrix");
	}
   status = umfpack_di_numeric(Fmatrix_unsymmetric_cols, Fmatrix_unsymmetric_indices, Fmatrix_unsymmetric, Symbolic, &Numeric, Control, Info);
   umfpack_di_free_symbolic(&Symbolic);

   status = umfpack_di_solve(UMFPACK_A, Fmatrix_unsymmetric_cols, Fmatrix_unsymmetric_indices, Fmatrix_unsymmetric, temp, Dvector, Numeric, Control, Info);

	if ((regularization_method != None) and ((vary_regularization_parameter) or (vary_pixel_fraction))) calculate_determinant = true; // specifies to calculate determinant

	if ((n_image_prior) or (max_sb_prior_unselected_pixels)) {
		max_pixel_sb=-1e30;
		int max_sb_i;
		for (int i=0; i < source_npixels; i++) {
			//if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_surface_brightness[i] = temp[i];
			if (source_surface_brightness[i] > max_pixel_sb) {
				max_pixel_sb = source_surface_brightness[i];
				max_sb_i = i;
			}
				//cout << source_surface_brightness[i] << " ";
		}
		if (n_image_prior) n_images_at_sbmax = source_pixel_n_images[max_sb_i];
	} else {
		for (int i=0; i < source_npixels; i++) {
			//if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_surface_brightness[i] = temp[i];
			//cout << source_surface_brightness[i] << " ";
		}
	}
	if (calculate_determinant) {
		double mantissa, exponent;
		status = umfpack_di_get_determinant (&mantissa, &exponent, Numeric, Info) ;
		if (status < 0) {
			die("WTF!");
		}
		//cout << "Fmatrix mantissa=" << mantissa << ", exponent=" << exponent << endl;
		Fmatrix_log_determinant = log(mantissa) + exponent*log(10);
		//cout << "Fmatrix_log_determinant = " << Fmatrix_log_determinant << endl;

		int Rmatrix_nonzero_elements = Rmatrix_index[source_npixels]-1;
		int Rmatrix_offdiags = Rmatrix_index[source_npixels]-1-source_npixels;
		int Rmatrix_unsymmetric_nonzero_elements = source_npixels + 2*Rmatrix_offdiags;
		//int Rmatrix_nonzero_elements = source_npixels + 2*Rmatrix_offdiags;
		if (Rmatrix_nonzero_elements==0) {
			cout << "nsource_pixels=" << source_npixels << endl;
			die("Rmatrix has zero size");
		}

		// Now we construct the transpose of Rmatrix so we can cast it into "unsymmetric" format for UMFPACK (by including offdiagonals on either side of diagonal elements)
		double *Rmatrix_transpose = new double[Rmatrix_nonzero_elements+1];
		int *Rmatrix_transpose_index = new int[Rmatrix_nonzero_elements+1];

		//int k,jl,jm,jp,ju,m,n2,noff,inc,iv;
		//double v;

		n2=Rmatrix_index[0];
		for (j=0; j < n2-1; j++) Rmatrix_transpose[j] = Rmatrix[j];
		n_offdiag = Rmatrix_index[n2-1] - Rmatrix_index[0];
		offdiag_indx = new int[n_offdiag];
		offdiag_indx_transpose = new int[n_offdiag];
		for (i=0; i < n_offdiag; i++) offdiag_indx[i] = Rmatrix_index[n2+i];
		indexx(offdiag_indx,offdiag_indx_transpose,n_offdiag);
		for (j=n2, k=0; j < Rmatrix_index[n2-1]; j++, k++) {
			Rmatrix_transpose_index[j] = offdiag_indx_transpose[k];
		}
		jp=0;
		for (k=Rmatrix_index[0]; k < Rmatrix_index[n2-1]; k++) {
			m = Rmatrix_transpose_index[k] + n2;
			Rmatrix_transpose[k] = Rmatrix[m];
			for (j=jp; j < Rmatrix_index[m]+1; j++)
				Rmatrix_transpose_index[j]=k;
			jp = Rmatrix_index[m] + 1;
			jl=0;
			ju=n2-1;
			while (ju-jl > 1) {
				jm = (ju+jl)/2;
				if (Rmatrix_index[jm] > m) ju=jm; else jl=jm;
			}
			Rmatrix_transpose_index[k]=jl;
		}
		for (j=jp; j < n2; j++) Rmatrix_transpose_index[j] = Rmatrix_index[n2-1];
		for (j=0; j < n2-1; j++) {
			jl = Rmatrix_transpose_index[j+1] - Rmatrix_transpose_index[j];
			noff=Rmatrix_transpose_index[j];
			inc=1;
			do {
				inc *= 3;
				inc++;
			} while (inc <= jl);
			do {
				inc /= 3;
				for (k=noff+inc; k < noff+jl; k++) {
					iv = Rmatrix_transpose_index[k];
					v = Rmatrix_transpose[k];
					m=k;
					while (Rmatrix_transpose_index[m-inc] > iv) {
						Rmatrix_transpose_index[m] = Rmatrix_transpose_index[m-inc];
						Rmatrix_transpose[m] = Rmatrix_transpose[m-inc];
						m -= inc;
						if (m-noff+1 <= inc) break;
					}
					Rmatrix_transpose_index[m] = iv;
					Rmatrix_transpose[m] = v;
				}
			} while (inc > 1);
		}
		delete[] offdiag_indx;
		delete[] offdiag_indx_transpose;

		int *Rmatrix_unsymmetric_cols = new int[source_npixels+1];
		int *Rmatrix_unsymmetric_indices = new int[Rmatrix_unsymmetric_nonzero_elements];
		double *Rmatrix_unsymmetric = new double[Rmatrix_unsymmetric_nonzero_elements];
		indx=0;
		Rmatrix_unsymmetric_cols[0] = 0;
		for (i=0; i < source_npixels; i++) {
			for (j=Rmatrix_transpose_index[i]; j < Rmatrix_transpose_index[i+1]; j++) {
				Rmatrix_unsymmetric[indx] = Rmatrix_transpose[j];
				Rmatrix_unsymmetric_indices[indx] = Rmatrix_transpose_index[j];
				indx++;
			}
			Rmatrix_unsymmetric_indices[indx] = i;
			Rmatrix_unsymmetric[indx] = Rmatrix[i];
			indx++;
			for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
				Rmatrix_unsymmetric[indx] = Rmatrix[j];
				//cout << "Row " << i << ", column " << Rmatrix_index[j] << ": " << Rmatrix[j] << " " << Rmatrix_unsymmetric[indx] << " (element " << indx << ")" << endl;
				Rmatrix_unsymmetric_indices[indx] = Rmatrix_index[j];
				indx++;
			}
			Rmatrix_unsymmetric_cols[i+1] = indx;
		}

		for (i=0; i < source_npixels; i++) {
			sort(Rmatrix_unsymmetric_cols[i+1]-Rmatrix_unsymmetric_cols[i],Rmatrix_unsymmetric_indices+Rmatrix_unsymmetric_cols[i],Rmatrix_unsymmetric+Rmatrix_unsymmetric_cols[i]);
			//cout << "Row " << i << ": " << endl;
			//cout << Rmatrix_unsymmetric_cols[i] << " ";
			//for (j=Rmatrix_unsymmetric_cols[i]; j < Rmatrix_unsymmetric_cols[i+1]; j++) {
				//cout << Rmatrix_unsymmetric_indices[j] << " ";
			//}
			//cout << endl;
			//for (j=Rmatrix_unsymmetric_cols[i]; j < Rmatrix_unsymmetric_cols[i+1]; j++) {
				//cout << Rmatrix_unsymmetric[j] << " ";
			//}
			//cout << endl;
		}
		//cout << endl;

		if (indx != Rmatrix_unsymmetric_nonzero_elements) die("WTF! Wrong number of nonzero elements");

		status = umfpack_di_symbolic(source_npixels, source_npixels, Rmatrix_unsymmetric_cols, Rmatrix_unsymmetric_indices, Rmatrix_unsymmetric, &Symbolic, Control, Info);
		if (status < 0) {
			umfpack_di_report_info (Control, Info) ;
			umfpack_di_report_status (Control, status) ;
			die("Error inputting matrix");
		}
		status = umfpack_di_numeric(Rmatrix_unsymmetric_cols, Rmatrix_unsymmetric_indices, Rmatrix_unsymmetric, Symbolic, &Numeric, Control, Info);
		if (status < 0) {
			umfpack_di_report_info (Control, Info) ;
			umfpack_di_report_status (Control, status) ;
			die("Error inputting matrix");
		}
		umfpack_di_free_symbolic(&Symbolic);

		status = umfpack_di_get_determinant (&mantissa, &exponent, Numeric, Info) ;
		//cout << "Rmatrix mantissa=" << mantissa << ", exponent=" << exponent << endl;
		if (status < 0) {
			die("Could not calculate determinant");
		}
		Rmatrix_log_determinant = log(mantissa) + exponent*log(10);
		//cout << "Rmatrix_logdet=" << Rmatrix_log_determinant << endl;
		delete[] Rmatrix_transpose;
		delete[] Rmatrix_transpose_index;
		delete[] Rmatrix_unsymmetric_cols;
		delete[] Rmatrix_unsymmetric_indices;
		delete[] Rmatrix_unsymmetric;
	}

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for inverting Fmatrix: " << wtime << endl;
	}
#endif

	delete[] temp;
	delete[] Fmatrix_transpose;
	delete[] Fmatrix_transpose_index;
	delete[] Fmatrix_unsymmetric_cols;
	delete[] Fmatrix_unsymmetric_indices;
	delete[] Fmatrix_unsymmetric;
	int index=0;
	source_pixel_grid->update_surface_brightness(index);
#endif
}

void Lens::invert_lens_mapping_MUMPS(bool verbal)
{
#ifdef USE_MPI
	MPI_Comm sub_comm;
	MPI_Comm_create(*group_comm, *mpi_group, &sub_comm);
#endif

#ifdef USE_MPI
	MPI_Comm this_comm;
	MPI_Comm_create(*my_comm, *my_group, &this_comm);
#endif

#ifndef USE_MUMPS
	die("QLens requires compilation with MUMPS for Cholesky factorization");
#else

	int default_nthreads=1;

#ifdef USE_OPENMP
	#pragma omp parallel
	{
		#pragma omp master
		default_nthreads = omp_get_num_threads();
	}
	omp_set_num_threads(inversion_nthreads);
#endif

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime0 = omp_get_wtime();
	}
#endif
	int i,j;

	double *temp = new double[source_npixels];
	MUMPS_INT Fmatrix_nonzero_elements = Fmatrix_index[source_npixels]-1;
	if (Fmatrix_nonzero_elements==0) {
		cout << "nsource_pixels=" << source_npixels << endl;
		die("Fmatrix has zero size");
	}
	MUMPS_INT *irn = new MUMPS_INT[Fmatrix_nonzero_elements];
	MUMPS_INT *jcn = new MUMPS_INT[Fmatrix_nonzero_elements];
	double *Fmatrix_elements = new double[Fmatrix_nonzero_elements];
	for (i=0; i < source_npixels; i++) {
		Fmatrix_elements[i] = Fmatrix[i];
		irn[i] = i+1;
		jcn[i] = i+1;
		temp[i] = Dvector[i];
	}
	int indx=source_npixels;
	for (i=0; i < source_npixels; i++) {
		for (j=Fmatrix_index[i]; j < Fmatrix_index[i+1]; j++) {
			Fmatrix_elements[indx] = Fmatrix[j];
			irn[indx] = i+1;
			jcn[indx] = Fmatrix_index[j]+1;
			indx++;
		}
	}

	if (use_mumps_subcomm)
		mumps_solver->comm_fortran=(MUMPS_INT) MPI_Comm_c2f(sub_comm);
	else
		mumps_solver->comm_fortran=(MUMPS_INT) MPI_Comm_c2f(this_comm);
	mumps_solver->job = JOB_INIT; // initialize
	mumps_solver->sym = 2; // specifies that matrix is symmetric and positive-definite
	//cout << "ICNTL = " << mumps_solver->icntl[13] << endl;
	dmumps_c(mumps_solver);
	mumps_solver->n = source_npixels; mumps_solver->nz = Fmatrix_nonzero_elements; mumps_solver->irn=irn; mumps_solver->jcn=jcn;
	mumps_solver->a = Fmatrix_elements; mumps_solver->rhs = temp;
	if (show_mumps_info) {
		mumps_solver->icntl[0] = MUMPS_OUTPUT;
		mumps_solver->icntl[1] = MUMPS_OUTPUT;
		mumps_solver->icntl[2] = MUMPS_OUTPUT;
		mumps_solver->icntl[3] = MUMPS_OUTPUT;
	} else {
		mumps_solver->icntl[0] = MUMPS_SILENT;
		mumps_solver->icntl[1] = MUMPS_SILENT;
		mumps_solver->icntl[2] = MUMPS_SILENT;
		mumps_solver->icntl[3] = MUMPS_SILENT;
	}
	if ((regularization_method != None) and ((vary_regularization_parameter) or (vary_pixel_fraction))) mumps_solver->icntl[32]=1; // specifies to calculate determinant
	else mumps_solver->icntl[32] = 0;
	if (parallel_mumps) {
		mumps_solver->icntl[27]=2; // parallel analysis phase
		mumps_solver->icntl[28]=2; // parallel analysis phase
	}
	mumps_solver->job = 6; // specifies to factorize and solve linear equation
#ifdef USE_MPI
	MPI_Barrier(sub_comm);
#endif
	dmumps_c(mumps_solver);
#ifdef USE_MPI
	if (use_mumps_subcomm) {
		MPI_Bcast(temp,source_npixels,MPI_DOUBLE,0,sub_comm);
		MPI_Barrier(sub_comm);
	}
#endif

	if (mumps_solver->info[0] < 0) {
		if (mumps_solver->info[0]==-10) die("Singular matrix, cannot invert");
		else warn("Error occurred during matrix inversion; MUMPS error code %i (source_npixels=%i)",mumps_solver->info[0],source_npixels);
	}

	if ((n_image_prior) or (max_sb_prior_unselected_pixels)) {
		max_pixel_sb=-1e30;
		int max_sb_i;
		for (int i=0; i < source_npixels; i++) {
			if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_surface_brightness[i] = temp[i];
			if (source_surface_brightness[i] > max_pixel_sb) {
				max_pixel_sb = source_surface_brightness[i];
				max_sb_i = i;
			}
		}
		if (n_image_prior) n_images_at_sbmax = source_pixel_n_images[max_sb_i];
	} else {
		for (int i=0; i < source_npixels; i++) {
			if ((data_pixel_noise==0) and (temp[i] < 0)) temp[i] = 0; // This might be a bad idea, but with zero noise there should be no negatives, and they annoy me when plotted
			source_surface_brightness[i] = temp[i];
		}
	}

	if ((regularization_method != None) and ((vary_regularization_parameter) or (vary_pixel_fraction)))
	{
		Fmatrix_log_determinant = log(mumps_solver->rinfog[11]) + mumps_solver->infog[33]*log(2);
		//cout << "Fmatrix log determinant = " << Fmatrix_log_determinant << endl;
		if ((mpi_id==0) and (verbal)) cout << "log determinant = " << Fmatrix_log_determinant << endl;

		mumps_solver->job=JOB_END; dmumps_c(mumps_solver); //Terminate instance

		MUMPS_INT Rmatrix_nonzero_elements = Rmatrix_index[source_npixels]-1;
		MUMPS_INT *irn_reg = new MUMPS_INT[Rmatrix_nonzero_elements];
		MUMPS_INT *jcn_reg = new MUMPS_INT[Rmatrix_nonzero_elements];
		double *Rmatrix_elements = new double[Rmatrix_nonzero_elements];
		for (i=0; i < source_npixels; i++) {
			Rmatrix_elements[i] = Rmatrix[i];
			irn_reg[i] = i+1;
			jcn_reg[i] = i+1;
		}
		indx=source_npixels;
		for (i=0; i < source_npixels; i++) {
			//cout << "Row " << i << ": diag=" << Rmatrix[i] << endl;
			//for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
				//cout << Rmatrix_index[j] << " ";
			//}
			//cout << endl;
			for (j=Rmatrix_index[i]; j < Rmatrix_index[i+1]; j++) {
				//cout << Rmatrix[j] << " ";
				Rmatrix_elements[indx] = Rmatrix[j];
				irn_reg[indx] = i+1;
				jcn_reg[indx] = Rmatrix_index[j]+1;
				indx++;
			}
		}

		mumps_solver->job=JOB_INIT; mumps_solver->sym=2;
		dmumps_c(mumps_solver);
		mumps_solver->n = source_npixels; mumps_solver->nz = Rmatrix_nonzero_elements; mumps_solver->irn=irn_reg; mumps_solver->jcn=jcn_reg;
		mumps_solver->a = Rmatrix_elements;
		mumps_solver->icntl[0]=MUMPS_SILENT;
		mumps_solver->icntl[1]=MUMPS_SILENT;
		mumps_solver->icntl[2]=MUMPS_SILENT;
		mumps_solver->icntl[3]=MUMPS_SILENT;
		mumps_solver->icntl[32]=1; // calculate determinant
		mumps_solver->icntl[30]=1; // discard factorized matrices
		if (parallel_mumps) {
			mumps_solver->icntl[27]=2; // parallel analysis phase
			mumps_solver->icntl[28]=2; // parallel analysis phase
		}
		mumps_solver->job=4;
		dmumps_c(mumps_solver);
		if (mumps_solver->rinfog[11]==0) Rmatrix_log_determinant = -1e20;
		else Rmatrix_log_determinant = log(mumps_solver->rinfog[11]) + mumps_solver->infog[33]*log(2);
		//cout << "Rmatrix log determinant = " << Rmatrix_log_determinant << " " << mumps_solver->rinfog[11] << " " << mumps_solver->infog[33] << endl;
		if ((mpi_id==0) and (verbal)) cout << "Rmatrix log determinant = " << Rmatrix_log_determinant << " " << mumps_solver->rinfog[11] << " " << mumps_solver->infog[33] << endl;

		delete[] irn_reg;
		delete[] jcn_reg;
		delete[] Rmatrix_elements;
	}
	mumps_solver->job=JOB_END;
	dmumps_c(mumps_solver); //Terminate instance

#ifdef USE_OPENMP
	if (show_wtime) {
		wtime = omp_get_wtime() - wtime0;
		if (mpi_id==0) cout << "Wall time for inverting Fmatrix: " << wtime << endl;
	}
#endif

#ifdef USE_OPENMP
	omp_set_num_threads(default_nthreads);
#endif

	delete[] temp;
	delete[] irn;
	delete[] jcn;
	delete[] Fmatrix_elements;
	int index=0;
	source_pixel_grid->update_surface_brightness(index);
#endif
#ifdef USE_MPI
	MPI_Comm_free(&sub_comm);
	MPI_Comm_free(&this_comm);
#endif

}

#define SWAP(a,b) temp=(a);(a)=(b);(b)=temp;
void Lens::indexx(int* arr, int* indx, int nn)
{
	const int M=7, NSTACK=50;
	int i,indxt,ir,j,k,jstack=-1,l=0;
	double a,temp;
	int *istack = new int[NSTACK];
	ir = nn - 1;
	for (j=0; j < nn; j++) indx[j] = j;
	for (;;) {
		if (ir-l < M) {
			for (j=l+1; j <= ir; j++) {
				indxt=indx[j];
				a=arr[indxt];
				for (i=j-1; i >=l; i--) {
					if (arr[indx[i]] <= a) break;
					indx[i+1]=indx[i];
				}
				indx[i+1]=indxt;
			}
			if (jstack < 0) break;
			ir=istack[jstack--];
			l=istack[jstack--];
		} else {
			k=(l+ir) >> 1;
			SWAP(indx[k],indx[l+1]);
			if (arr[indx[l]] > arr[indx[ir]]) {
				SWAP(indx[l],indx[ir]);
			}
			if (arr[indx[l+1]] > arr[indx[ir]]) {
				SWAP(indx[l+1],indx[ir]);
			}
			if (arr[indx[l]] > arr[indx[l+1]]) {
				SWAP(indx[l],indx[l+1]);
			}
			i=l+1;
			j=ir;
			indxt=indx[l+1];
			a=arr[indxt];
			for (;;) {
				do i++; while (arr[indx[i]] < a);
				do j--; while (arr[indx[j]] > a);
				if (j < i) break;
				SWAP(indx[i],indx[j]);
			}
			indx[l+1]=indx[j];
			indx[j]=indxt;
			jstack += 2;
			if (jstack >= NSTACK) die("NSTACK too small in indexx");
			if (ir-i+1 >= j-l) {
				istack[jstack]=ir;
				istack[jstack-1]=i;
				ir=j-1;
			} else {
				istack[jstack]=j-1;
				istack[jstack-1]=l;
				l=i;
			}
		}
	}
	delete[] istack;
}
#undef SWAP(a,b)



void Lens::clear_lensing_matrices()
{
	if (Dvector != NULL) delete[] Dvector;
	if (Fmatrix != NULL) delete[] Fmatrix;
	if (Fmatrix_index != NULL) delete[] Fmatrix_index;
	if (Rmatrix != NULL) delete[] Rmatrix;
	if (Rmatrix_index != NULL) delete[] Rmatrix_index;
	Dvector = NULL;
	Fmatrix = NULL;
	Fmatrix_index = NULL;
	Rmatrix = NULL;
	Rmatrix_index = NULL;
}

void Lens::calculate_image_pixel_surface_brightness()
{
	int i,j;
	for (i=0; i < image_npixels; i++) {
		image_surface_brightness[i] = 0;
		for (j=image_pixel_location_Lmatrix[i]; j < image_pixel_location_Lmatrix[i+1]; j++) {
			image_surface_brightness[i] += Lmatrix[j]*source_surface_brightness[Lmatrix_index[j]];
		}
	}
}

void Lens::store_image_pixel_surface_brightness()
{
	int i,j;
	for (i=0; i < image_pixel_grid->x_N; i++)
		for (j=0; j < image_pixel_grid->y_N; j++)
			image_pixel_grid->surface_brightness[i][j] = 0;

	for (int img_index=0; img_index < image_npixels; img_index++) {
		i = active_image_pixel_i[img_index];
		j = active_image_pixel_j[img_index];
		image_pixel_grid->surface_brightness[i][j] = image_surface_brightness[img_index];
	}
}

void Lens::plot_image_pixel_surface_brightness(string outfile_root)
{
	string sb_filename = outfile_root + ".dat";
	string x_filename = outfile_root + ".x";
	string y_filename = outfile_root + ".y";

	ofstream xfile(x_filename.c_str());
	for (int i=0; i <= image_pixel_grid->x_N; i++) {
		xfile << image_pixel_grid->corner_pts[i][0][0] << endl;
	}

	ofstream yfile(y_filename.c_str());
	for (int i=0; i <= image_pixel_grid->y_N; i++) {
		yfile << image_pixel_grid->corner_pts[0][i][1] << endl;
	}

	ofstream surface_brightness_file(sb_filename.c_str());
	int index=0;
	index=0;
	for (int j=0; j < image_pixel_grid->y_N; j++) {
		for (int i=0; i < image_pixel_grid->x_N; i++) {
			if (image_pixel_grid->maps_to_source_pixel[i][j])
				surface_brightness_file << image_surface_brightness[index++] << " ";
			else surface_brightness_file << "0 ";
		}
		surface_brightness_file << endl;
	}
}

