#pragma once

#include <vector>
#include "Marker.h"

// This class declaration is for an immersed boundary Lagrange point.
// A collection of these points form an immersed boundary body.
class IBMarker : public Marker {

	// Make ObjectManager a friend class so it can access the protected data of IBMarker objects
	friend class ObjectManager;
	// Same for IBBody
	friend class IBBody;

public:

	// Default constructor and destructor
	IBMarker(void)
	{
	};
	~IBMarker(void)
	{
	};

	// Custom constructor
	IBMarker(double xPos, double yPos, double zPos, bool flex_rigid);

protected:

	/*	
	***************************************************************************************************************
	********************************************* Member Data *****************************************************
	***************************************************************************************************************
	*/
	
	// General vectors
	std::vector<double> fluid_vel;		// Fluid velocity interpolated from lattice nodes
	std::vector<double> desired_vel;	// Desired velocity
	std::vector<double> force_xyz;		// Restorative force vector on marker
	std::vector<double> position_old;	// Used for filaments: Vector containing the physical coordinates (x,y,z) of the marker at t-1

	// Support quantities
	std::vector<double> deltaval;		// Value of delta function for a given support node

	// Scalars
	bool flex_rigid;		// false == rigid/fixed; true == flexible/moving
	double epsilon;			// Scaling parameter
	double local_area;		// Area associated with support node in lattice units (same for all points if from same grid and regularly spaced like LBM)
	double dilation;		// Dilation parameter in lattice units (same in all directions for uniform Eulerian grid)

};

