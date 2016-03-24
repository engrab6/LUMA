/* This file holds all the code for the core LBM operations including collision,
streaming and macroscopic calulcation.
*/

#include "../inc/stdafx.h"
#include "../inc/GridObj.h"
#include "../inc/definitions.h"
#include "../inc/globalvars.h"
#include "../inc/IVector.h"
#include "../inc/ObjectManager.h"
#include "../inc/MpiManager.h"

using namespace std;

// ***************************************************************************************************
// LBM multi-grid kernel applicable for both single and multi-grid (IBM assumed on coarse grid for now)
// IBM_flag dictates whether we need the predictor step or not
void GridObj::LBM_multi ( bool IBM_flag ) {

	// Start the clock to time the kernel
	clock_t secs, t_start = clock();


	///////////////////////////////
	// IBM pre-kernel processing //
	///////////////////////////////


	// Copy distributions prior to IBM predictive step
#ifdef IBM_ON
	// Local stores used to hold info prior to IBM predictive step
	IVector<double> f_ibm_initial, u_ibm_initial, rho_ibm_initial;

	// If IBM on and predictive loop flag true then store initial data and reset forces
	if (level == 0 && IBM_flag == true) { // Limit to level 0 immersed body for now

		*GridUtils::logfile << "Prediction step..." << std::endl;

		// Store lattice data
		f_ibm_initial = f;
		u_ibm_initial = u;
		rho_ibm_initial = rho;

		// Reset lattice and Cartesian force vectors at each site
		LBM_forcegrid(true);
	}
#else

	// If not using IBM then just reset the force vectors
	LBM_forcegrid(true);

#endif



	////////////////
	// LBM kernel //
	////////////////


	// Loop twice on refined levels as refinement ratio per level is 2
	int count = 1;
	do {

		// Apply boundary conditions (regularised must be applied before collision)
#if (defined INLET_ON && defined INLET_REGULARISED && !defined INLET_DO_NOTHING)
		LBM_boundary(2);
#endif

#ifdef MEGA_DEBUG
		/*DEBUG*/ io_tecplot_debug((t+1)*100 + 0,"AFTER INLET BC");
#endif

		// Force lattice directions using current Cartesian force vector (adding gravity if necessary)
		LBM_forcegrid(false);

		// Collision on Lr
		LBM_collide();

#ifdef MEGA_DEBUG
		/*DEBUG*/ io_tecplot_debug((t+1)*100 + 1,"AFTER COLLIDE");
#endif

		////////////////////
		// Refined levels //
		////////////////////

		// Check if lower level expected
		if (NumLev > level) {

			size_t regions = subGrid.size();
			for (size_t reg = 0; reg < regions; reg++) {

				// Explode
				LBM_explode(reg);

				// Call same routine for lower level
				subGrid[reg].LBM_multi(IBM_flag);

			}

			// Apply boundary conditions
#if ( (defined INLET_ON || defined OUTLET_ON) && (!defined INLET_DO_NOTHING && !defined INLET_REGULARISED) )
			LBM_boundary(2);	// Inlet (Zou-He)
#endif
#if (defined SOLID_BLOCK_ON || defined WALLS_ON)
			LBM_boundary(1);	// Bounce-back (walls and solids)
#endif

#ifdef MEGA_DEBUG
			/*DEBUG*/ io_tecplot_debug((t+1)*100 + 2,"AFTER SOLID BC");
#endif

#ifdef BFL_ON
			// Store the f values pre stream for BFL
			ObjectManager::getInstance()->f_prestream = f;
#endif

			// Stream
			LBM_stream();

#ifdef MEGA_DEBUG
			/*DEBUG*/ io_tecplot_debug((t+1)*100 + 3,"AFTER STREAM");
#endif

			// Apply boundary conditions
#ifdef BFL_ON
			LBM_boundary(5);	// BFL boundary conditions
#endif
#ifdef MEGA_DEBUG
			/*DEBUG*/ io_tecplot_debug((t+1)*100 + 4,"AFTER BFL");
#endif

			for (size_t reg = 0; reg < regions; reg++) {

				// Coalesce
				LBM_coalesce(reg);

			}

#ifdef MEGA_DEBUG
			/*DEBUG*/ io_tecplot_debug((t+1)*100 + 5,"AFTER COALESCE"); // Do not change this tag!
#endif


			///////////////////////
			// No refined levels //
			///////////////////////

		} else {

			// Apply boundary conditions
#if ( (defined INLET_ON || defined OUTLET_ON) && (!defined INLET_DO_NOTHING && !defined INLET_REGULARISED) )
			LBM_boundary(2);	// Inlet (Zou-He)
#endif
#if (defined SOLID_BLOCK_ON || defined WALLS_ON)
			LBM_boundary(1);	// Bounce-back (walls and solids)
#endif

#ifdef MEGA_DEBUG
			/*DEBUG*/ io_tecplot_debug((t+1)*100 + 2,"AFTER SOLID BC");
#endif

#ifdef BFL_ON
			// Store the f values pre stream for BFL
			ObjectManager::getInstance()->f_prestream = f;
#endif

			// Stream
			LBM_stream();

#ifdef MEGA_DEBUG
			/*DEBUG*/ io_tecplot_debug((t+1)*100 + 3,"AFTER STREAM");
#endif


			// Apply boundary conditions
#ifdef BFL_ON
			LBM_boundary(5);	// BFL boundary conditions
#endif
#ifdef MEGA_DEBUG
			/*DEBUG*/ io_tecplot_debug((t+1)*100 + 4,"AFTER BFL");
#endif

		}


		//////////////
		// Continue //
		//////////////

		// Apply boundary conditions
#ifdef OUTLET_ON
		LBM_boundary(3);	// Outlet
#endif

#ifdef MEGA_DEBUG
		/*DEBUG*/ io_tecplot_debug((t+1)*100 + 6,"AFTER OUTLET BC");
#endif

		// Update macroscopic quantities (including time-averaged quantities)
		LBM_macro();

#ifdef MEGA_DEBUG
		/*DEBUG*/ io_tecplot_debug((t+1)*100 + 7,"AFTER MACRO");
#endif

		// Check if on L0 and if so drop out as only need to loop once on coarsest level
		if (level == 0) {
			// Increment time step counter and break
			t++;
			break;
		}

		// Increment counters
		t++; count++;

	} while (count < 3);




	////////////////////////////////
	// IBM post-kernel processing //
	////////////////////////////////

	// Execute IBM procedure using newly computed predicted data
#ifdef IBM_ON
	if (level == 0 && IBM_flag == true) {

		// Reset force vectors on grid in preparation for spreading step
		LBM_forcegrid(true);


		// Calculate and apply IBM forcing to fluid
		ObjectManager::getInstance()->ibm_apply(*this);

		// Restore data to start of time step
		f = f_ibm_initial;
		u = u_ibm_initial;
		rho = rho_ibm_initial;

		// Relaunch kernel with IBM flag set to false (corrector step)
		// Corrector step does not reset force vectors but uses newly computed vector instead.
		*GridUtils::logfile << "Correction step..." << std::endl;
		t--;                // Predictor-corrector results in double time step (need to reset back 1)
		LBM_multi(false);

		// Move the body if necessary
		ObjectManager::getInstance()->ibm_move_bodies(*this);

	}
#endif

	// Get time of loop (includes sub-loops)
	secs = clock() - t_start;

	// Update average timestep time on this grid
	timeav_timestep *= (t-1);
	timeav_timestep += ((double)secs)/CLOCKS_PER_SEC;
	timeav_timestep /= t;

	if (t % out_every == 0) {
		// Performance data to logfile
		*GridUtils::logfile << "Time stepping taking an average of " << timeav_timestep*1000 << "ms" << std::endl;
	}


	///////////////////////
	// MPI communication //
	///////////////////////

#ifdef BUILD_FOR_MPI
	/* Do MPI communication on this grid level before returning. */

	// Launch communication on this grid by passing its level and region number
	MpiManager::getInstance()->mpi_communicate(level, region_number);

#endif


}

// ***************************************************************************************************
// Takes Cartesian force vector and populates forces for each lattice direction.
// If reset_flag is true, resets the force vectors.
void GridObj::LBM_forcegrid(bool reset_flag) {

	/* This routine computes the forces applied along each direction on the lattice
	from Guo's 2002 scheme. The basic LBM must be modified in two ways: 1) the forces
	are added to the populations produced by the LBGK collision in the collision
	routine; 2) dt/2 * F is added to the momentum in the macroscopic calculation. This
	has been done already in the other routines.

	The forces along each direction are computed according to:

	F_i = (1 - 1/2*tau) * (w_i / cs^2) * ( (c_i - v) + (1/cs^2) * (c_i . v) * c_i ) . F

	where
	F_i = force applied to lattice in i-th direction
	tau = relaxation time
	w_i = weight in i-th direction
	cs = lattice sound speed
	c_i = vector of lattice speeds for i-th direction
	v = macroscopic velocity vector
	F = Cartesian force vector

	In the following, we assume
	lambda_i = (1 - 1/2*tau) * (w_i / cs^2)
	beta_i = (1/cs^2) * (c_i . v)

	The above in shorthand becomes summing over d dimensions:

	F_i = lambda_i * sum( F_d * (c_d_i (1+beta_i) - u_d )

	*/

	if (reset_flag) {

		// Reset lattice force vectors on every grid site
		std::fill(force_i.begin(), force_i.end(), 0.0);

		// Reset Cartesian force vector on every grid site
		std::fill(force_xyz.begin(), force_xyz.end(), 0.0);

	} else {
	// Else, compute forces

		// Get grid sizes
		size_t N_lim = XPos.size();
		size_t M_lim = YPos.size();
		size_t K_lim = ZPos.size();

		// Declarations
		double lambda_v;

		// Loop over grid and overwrite forces for each direction
		for (size_t i = 0; i < N_lim; i++) {
			for (size_t j = 0; j < M_lim; j++) {
				for (size_t k = 0; k < K_lim; k++) {

#ifdef GRAVITY_ON
					// Add gravity to any IBM forces currently stored
					force_xyz(i,j,k,grav_direction,M_lim,K_lim,dims) += rho(i,j,k,M_lim,K_lim) * grav_force;
#endif

					// Now compute force_i components from Cartesian force vector
					for (size_t v = 0; v < nVels; v++) {

						// Only apply to non-solid sites
						if (LatTyp(i,j,k,M_lim,K_lim) != 0) {

							// Reset beta_v
							double beta_v = 0.0;

							// Compute the lattice forces based on Guo's forcing scheme
							lambda_v = (1 - 0.5 * omega) * ( w[v] / pow(cs,2) );

							// Dot product (sum over d dimensions)
							for (int d = 0; d < dims; d++) {
								beta_v +=  (c[d][v] * u(i,j,k,d,M_lim,K_lim,dims));
							}
							beta_v = beta_v * (1/pow(cs,2));

							// Compute force using shorthand sum described above
							for (int d = 0; d < dims; d++) {
								force_i(i,j,k,v,M_lim,K_lim,nVels) += force_xyz(i,j,k,d,M_lim,K_lim,dims) *
									(c[d][v] * (1 + beta_v) - u(i,j,k,d,M_lim,K_lim,dims));
							}

							// Multiply by lambda_v
							force_i(i,j,k,v,M_lim,K_lim,nVels) = force_i(i,j,k,v,M_lim,K_lim,nVels) * lambda_v;

						}

					}

				}
			}
		}


#ifdef IBM_DEBUG
		// DEBUG -- write out force components
		std::ofstream testout;
		testout.open(GridUtils::path_str + "/force_i_LB.out", std::ios::app);
		testout << "\nNEW TIME STEP" << std::endl;
		for (size_t j = 1; j < M_lim - 1; j++) {
			for (size_t i = 0; i < N_lim; i++) {
				for (size_t v = 0; v < nVels; v++) {
					testout << force_i(i,j,0,v,M_lim,K_lim,nVels) << "\t";
				}
				testout << std::endl;
			}
			testout << std::endl;
		}
		testout.close();
#endif
	}

}


// ***************************************************************************************************
// Collision operator
void GridObj::LBM_collide( ) {

	/*
	Loop through the lattice points to compute the new distribution functions.
	Equilibrium based on:
	       rho * w * (1 + c_ia u_a / cs^2 + Q_iab u_a u_b / 2*cs^4)
	*/

	// Declarations and Grid size
	int N_lim = XPos.size();
	int M_lim = YPos.size();
	int K_lim = ZPos.size();

	// Create temporary lattice to prevent overwriting useful populations and initialise with same values as
	// pre-collision f grid. Initialise with current f values.
	IVector<double> f_new( f );


	// Loop over all lattice sites
	for (int i = 0; i < N_lim; i++) {
		for (int j = 0; j < M_lim; j++) {
			for (int k = 0; k < K_lim; k++) {

				// Ignore refined sites and TL sites (based on Rohde refinement)
				if (LatTyp(i,j,k,M_lim,K_lim) == 2 || LatTyp(i,j,k,M_lim,K_lim) == 3) {
					// Do nothing as taken care of on lower level grid

				} else {


#ifdef USE_MRT
					// Call MRT collision for given lattice site
					LBM_mrt_collide( f_new, i, j, k);
#else
					// Loop over directions and perform collision
					for (int v = 0; v < nVels; v++) {

						// Get feq value by calling overload of collision function
						feq(i,j,k,v,M_lim,K_lim,nVels) = LBM_collide( i, j, k, v );

						// Recompute distribution function f
						f_new(i,j,k,v,M_lim,K_lim,nVels) = ( -omega * (f(i,j,k,v,M_lim,K_lim,nVels) - feq(i,j,k,v,M_lim,K_lim,nVels)) )
															+ f(i,j,k,v,M_lim,K_lim,nVels)
															+ force_i(i,j,k,v,M_lim,K_lim,nVels);

					}
#endif

				}

			}
		}
	}

	// Update f from fnew
	f = f_new;

}

// ***************************************************************************************************
// Overload of collision function to allow calculation of feq only for initialisation
double GridObj::LBM_collide( int i, int j, int k, int v ) {

	/* LBGK equilibrium function is represented as:
		feq_i = rho * w_i * ( 1 + u_a c_ia / cs^2 + Q_iab u_a u_b / 2*cs^4 )
	where
		Q_iab = c_ia c_ib - cs^2 * delta_ab
	and
		delta_ab is the Kronecker delta.
	*/

	// Declare single feq value and intermediate values A and B
	double feq, A, B;

	// Other declarations
	int M_lim = YPos.size();
	int K_lim = ZPos.size();

	// Compute the parts of the expansion for feq (we now have a dot product routine so could simplify this code)

#if (dims == 3)
		// Compute c_ia * u_a which is actually the dot product of c and u
		A = (c[0][v] * u(i,j,k,0,M_lim,K_lim,dims)) + (c[1][v] * u(i,j,k,1,M_lim,K_lim,dims)) + (c[2][v] * u(i,j,k,2,M_lim,K_lim,dims));

		/*
		Compute second term in the expansion
		Q_iab u_a u_b =
		(c_x^2 - cs^2)u_x^2 + (c_y^2 - cs^2)u_y^2 + (c_z^2 - cs^2)u_z^2
		+ 2c_x c_y u_x u_y + 2c_x c_z u_x u_z + 2c_y c_z u_y u_z
		*/

		B =	(pow(c[0][v],2) - pow(cs,2)) * pow(u(i,j,k,0,M_lim,K_lim,dims),2) +
			(pow(c[1][v],2) - pow(cs,2)) * pow(u(i,j,k,1,M_lim,K_lim,dims),2) +
			(pow(c[2][v],2) - pow(cs,2)) * pow(u(i,j,k,2,M_lim,K_lim,dims),2) +
			2 * c[0][v]*c[1][v] * u(i,j,k,0,M_lim,K_lim,dims) * u(i,j,k,1,M_lim,K_lim,dims) +
			2 * c[0][v]*c[2][v] * u(i,j,k,0,M_lim,K_lim,dims) * u(i,j,k,2,M_lim,K_lim,dims) +
			2 * c[1][v]*c[2][v] * u(i,j,k,1,M_lim,K_lim,dims) * u(i,j,k,2,M_lim,K_lim,dims);
#else
		// 2D versions of the above
		A = (c[0][v] * u(i,j,k,0,M_lim,K_lim,dims)) + (c[1][v] * u(i,j,k,1,M_lim,K_lim,dims));

		B =	(pow(c[0][v],2) - pow(cs,2)) * pow(u(i,j,k,0,M_lim,K_lim,dims),2) +
			(pow(c[1][v],2) - pow(cs,2)) * pow(u(i,j,k,1,M_lim,K_lim,dims),2) +
			2 * c[0][v]*c[1][v] * u(i,j,k,0,M_lim,K_lim,dims) * u(i,j,k,1,M_lim,K_lim,dims);
#endif


	// Compute f^eq
	feq = rho(i,j,k,M_lim,K_lim) * w[v] * ( 1 + (A / pow(cs,2)) + (B / (2*pow(cs,4))) );

	return feq;

}


// ***************************************************************************************************
// MRT collision procedure for site (i,j,k).
void GridObj::LBM_mrt_collide( IVector<double>& f_new, int i, int j, int k ) {
#ifdef USE_MRT

	// Size declarations
	int M_lim = YPos.size();
	int K_lim = ZPos.size();

	// Temporary vectors
	std::vector<double> m;					// Vector of moments
	m.resize(nVels);
	std::fill(m.begin(), m.end(), 0.0);		// Set to zero
	std::vector<double> meq( m );			// Vector of equilibrium moments

	// Loop over directions and update equilibrium function
	for (int v = 0; v < nVels; v++) {

		// Get feq value by calling overload of collision function
		feq(i,j,k,v,M_lim,K_lim,nVels) = LBM_collide( i, j, k, v );

	}


	/* Compute the moment vectors using forward transformation to moment space
	 *
	 * m_p = M_pq f_q
	 * m_eq_p = M_pq f_eq_q
	 *
	 */

	// Do a matrix * vector operation
	for (int p = 0; p < nVels; p++) {
		for (int q = 0; q < nVels; q++) {

			m[p] += mMRT[p][q] * f(i,j,k,q,M_lim,K_lim,nVels);
			meq[p] += mMRT[p][q] * feq(i,j,k,q,M_lim,K_lim,nVels);

		}
	}


	/* Perform the collision in moment space for a component q
	 *
	 * m_new_q = m_q - s_q(m_q - m_eq_q)
	 *
	 * where s_q is the relaxation rate for component q.
	 *
	 */

	// Overwrite old moments
	double mtmp;
	for (int q = 0; q < nVels; q++) {

		mtmp = m[q] - mrt_omega[q] * (m[q] - meq[q]);
		m[q] = mtmp;

	}



	/* Get populations from the moments by transforming back to velocity space
	 *
	 * f_i_new = M^-1_ij * m_new_j
	 *
	 */

	double ftmp;
	// Do a matrix * vector operation
	for (int p = 0; p < nVels; p++) {
		ftmp = 0.0;

		for (int q = 0; q < nVels; q++) {

			ftmp += mInvMRT[p][q] * m[q];

		}

		f_new(i,j,k,p,M_lim,K_lim,nVels) = ftmp;

	}

#endif
}

// ***************************************************************************************************
// Streaming operator
// Applies periodic BCs on level 0
void GridObj::LBM_stream( ) {

	/* This streaming operation obeys the following logic process:
	 *
	 *	1) Apply Source-based Exclusions (e.g. any fine sites does not need to be streamed)
	 *
	 * Then either:
	 *	2a) Apply Off-Grid Ops (e.g. retain incoming values if value streams off-grid or apply periodic BCs)
	 *
	 * Or
	 *	2b) Apply Destination-based Exclusions (e.g. do not stream to a do-nothing inlet)
	 *	2c) Stream value
	 *
	 * End
	 */

	// Declarations
	int N_lim = XPos.size();
	int M_lim = YPos.size();
	int K_lim = ZPos.size();
	int dest_x, dest_y, dest_z;
	int v_opp;

	// Create temporary lattice of zeros to prevent overwriting useful populations
	IVector<double> f_new( f.size(), 0.0 );	// Could just initialise to f to make the logic below simpler //


#ifdef DEBUG_STREAM
	/*DEBUG*/
	int count0 = 0, count1 = 0, count2 = 0, count3 = 0;
#endif

	// Stream one lattice site at a time
	for (int i = 0; i < N_lim; i++) {
		for (int j = 0; j < M_lim; j++) {
			for (int k = 0; k < K_lim; k++) {

				for (int v = 0; v < nVels; v++) {

					// Store opposite direction
					v_opp = GridUtils::getOpposite(v);

#ifdef DEBUG_STREAM
					/*DEBUG*/
					count0++;
#endif


					/////////////////////////////////
					// Streaming Source Exclusions //
					/////////////////////////////////

					/* This section prevents streaming operations given the type of site
					 * FROM which the streaming takes place regardless of the destination type.
					 */

					// Fine --> Any; do not stream in any direction
					if (LatTyp(i,j,k,M_lim,K_lim) == 2) {
						break;
					}

					// Do-nothing-inlet --> Any; copy value to new grid (i.e. apply do-nothing inlet)
#if (defined INLET_ON && defined INLET_DO_NOTHING)
					else if (LatTyp(i,j,k,M_lim,K_lim) == 7) {
						f_new(i,j,k,v,M_lim,K_lim,nVels) = f(i,j,k,v,M_lim,K_lim,nVels);
						continue;
					}
#endif


					////////////////////////
					// Off-grid streaming //
					////////////////////////

					/* If destination off-grid then ask whether periodic boundaries in use
					 * and if so then only stream if Coarse --> Coarse.
					 * If not then retain the incoming value at the site as it will not receive
					 * an update from off-grid.
					 * If using MPI, periodic BCs are applied differently later.
					 */

					// Compute destination site (no periodicity)
					dest_x = i+c[0][v];
					dest_y = j+c[1][v];
					dest_z = k+c[2][v];


					// If off-grid
					if (	(dest_x >= N_lim || dest_x < 0) ||
							(dest_y >= M_lim || dest_y < 0)
#if (dims == 3)
							|| (dest_z >= K_lim || dest_z < 0)
#endif
						) {


#ifdef DEBUG_STREAM
							/*DEBUG*/
							count1++;
							*GridUtils::logfile << "Stream " << i << "," << j << "," << k << 
								" (" << XPos[i] << "," << YPos[j] << "," << ZPos[k] << ")" <<
								" to \t" << dest_x << "," << dest_y << "," << dest_z <<
								" : \toff-grid in " <<
								v << " direction. Count1 = " << count1 << ". Value is f = " 
								<< f(i,j,k,v,M_lim,K_lim,nVels) << std::endl;
#endif



							// Apply periodic boundary conditions (serial/non-MPI)
#if (defined PERIODIC_BOUNDARIES  && !defined BUILD_FOR_MPI)

							// Compute destination site indices using periodicity
							dest_x = (i+c[0][v] + N_lim) % N_lim;
							dest_y = (j+c[1][v] + M_lim) % M_lim;
							dest_z = (k+c[2][v] + K_lim) % K_lim;

							// Only apply periodic BCs on coarsest level and if stream is Coarse --> Coarse
							if (
								(level == 0) &&
								(LatTyp(i,j,k,M_lim,K_lim) == 1 && LatTyp(dest_x,dest_y,dest_z,M_lim,K_lim) == 1)
							) {
								// Stream periodically
								f_new(dest_x,dest_y,dest_z,v,M_lim,K_lim,nVels) = f(i,j,k,v,M_lim,K_lim,nVels);
								continue;	// Move on to next site
							}

#endif

							// If not using periodic boundary conditions then retain incoming value as 
							// will not receive an update from off-grid and continue
							f_new(i,j,k,v_opp,M_lim,K_lim,nVels) = f(i,j,k,v_opp,M_lim,K_lim,nVels);
							continue;



					} else {


						///////////////////////
						// On-grid streaming //
						///////////////////////


						/* If it is an on-grid stream and using MPI need some
						 * additional logic checking to make sure we prevent stream
						 * from a periodic recv layer site when periodic BCs are not in effect. */
						
#ifdef BUILD_FOR_MPI

						//////////////////////
						// MPI Periodic BCs //
						//////////////////////

						/* If source in recv layer and destination in sender layer 
						 * check if this is a periodic stream. If periodic BCs are 
						 * in use then allow stream. If not then do not stream from 
						 * recv layer to sender layer as this would constitute an 
						 * illegal periodic stream. */
						if (

						(

						// Condition 1: Source in a recv layer
						GridUtils::isOnRecvLayer(XPos[i],YPos[j],ZPos[k])

						) && (

						// Condition 2: Destination on-grid (in a sender layer)
						GridUtils::isOnSenderLayer(XPos[dest_x],YPos[dest_y],ZPos[dest_z])
						

						) && (

						// Condition 3: Recv layer is linked to a periodic neighbour rank
						GridUtils::isOverlapPeriodic(i,j,k,*this)

						)

						) {


#ifdef DEBUG_STREAM
							/*DEBUG*/
							count2++;
							*GridUtils::logfile << "Stream " << i << "," << j << "," << k << 
								" (" << XPos[i] << "," << YPos[j] << "," << ZPos[k] << ")" <<
								" to \t" << dest_x << "," << dest_y << "," << dest_z <<
								" (" << XPos[dest_x] << "," << YPos[dest_y] << "," << ZPos[dest_z] << ")" << 
								" : \tperiodic stream " <<
									v << " direction. Count2 = " << count2 << ". Value is f = " 
									<< f(i,j,k,v,M_lim,K_lim,nVels) << std::endl;
#endif


						// Either apply periodic BCs or not on these sites (ones which stream from periodic recv site)

#ifdef PERIODIC_BOUNDARIES
							if (LatTyp(i,j,k,M_lim,K_lim) == 1 && LatTyp(dest_x,dest_y,dest_z,M_lim,K_lim) == 1)
							{
								// Stream periodically
								f_new(dest_x,dest_y,dest_z,v,M_lim,K_lim,nVels) = f(i,j,k,v,M_lim,K_lim,nVels);
								continue;

							} else {
								// Incoming value at the destination site should be retained as no periodic BC to be applied
								f_new(dest_x,dest_y,dest_z,v,M_lim,K_lim,nVels) = f(dest_x,dest_y,dest_z,v,M_lim,K_lim,nVels);
								continue;

							}


						}
#else

							// If not using periodic BCs then incoming value at the destination site should be retained
							f_new(dest_x,dest_y,dest_z,v,M_lim,K_lim,nVels) = f(dest_x,dest_y,dest_z,v,M_lim,K_lim,nVels);
							continue;

						}

#endif	// PERIODIC_BOUNDARIES

#endif	// BUILD_FOR_MPI




						//////////////////////////////////////
						// Streaming Destination Exclusions //
						//////////////////////////////////////

						/* Filter out unwanted streaming operations by checking the
						 * source-destination pairings and retaining those values
						 * that you do not want to be overwritten by streaming.
						 * This section prevents streaming operations given the type of site
						 * TO which the streaming takes place regardless of the source type.
						 */

						// TL2lower --> TL2lower then ignore as done on lower grid stream
						if (
							(LatTyp(i,j,k,M_lim,K_lim) == 4) &&
							(LatTyp(dest_x,dest_y,dest_z,M_lim,K_lim) == 4)
						) {
							continue;

						}

						// Any --> Do-nothing-inlet then ignore so as not to overwrite
#if (defined INLET_ON && defined INLET_DO_NOTHING)
						else if (LatTyp(dest_x,dest_y,dest_z,M_lim,K_lim) == 7) {
							continue;
						}
#endif

#ifdef DEBUG_STREAM
						/*DEBUG*/
						count3++;
						*GridUtils::logfile << "Stream " << i << "," << j << "," << k << 
								" (" << XPos[i] << "," << YPos[j] << "," << ZPos[k] << ")" <<
								" to \t" << dest_x << "," << dest_y << "," << dest_z <<
								" (" << XPos[dest_x] << "," << YPos[dest_y] << "," << ZPos[dest_z] << ")" << 
								" : \ton-grid stream " <<
								v << " direction. Count3 = " << count3 << ". Value is f = " 
								<< f(i,j,k,v,M_lim,K_lim,nVels) << std::endl;
#endif


						// Stream population
						f_new(dest_x,dest_y,dest_z,v,M_lim,K_lim,nVels) = f(i,j,k,v,M_lim,K_lim,nVels);

					}


				}

			}
		}
	}


#ifdef DEBUG_STREAM
	/*DEBUG*/
	*GridUtils::logfile << "Counts were " << count0 << "," << count1 << "," << count2 << "," << count3 << std::endl;
#endif

	// Replace old grid with new grid
	f = f_new;

}


// ***************************************************************************************************
// Macroscopic quantity calculation and update of time-averaged quantities
void GridObj::LBM_macro( ) {

	// Declarations
	int N_lim = XPos.size();
	int M_lim = YPos.size();
	int K_lim = ZPos.size();
	double rho_temp = 0.0;
	double fux_temp = 0.0;
	double fuy_temp = 0.0;
	double fuz_temp = 0.0;
	double ta_temp = 0.0;


	// Loop over lattice
	for (int i = 0; i < N_lim; i++) {
		for (int j = 0; j < M_lim; j++) {
			for (int k = 0; k < K_lim; k++) {

				if (LatTyp(i,j,k,M_lim,K_lim) == 2) {

					// Refined site so set both density and velocity to zero
					rho(i,j,k,M_lim,K_lim) = 0.0;
					u(i,j,k,0,M_lim,K_lim,dims) = 0.0;
					u(i,j,k,1,M_lim,K_lim,dims) = 0.0;
#if (dims == 3)
					u(i,j,k,2,M_lim,K_lim,dims) = 0.0;
#endif

				} else if (LatTyp(i,j,k,M_lim,K_lim) == 0 || LatTyp(i,j,k,M_lim,K_lim) == 5) {

					// Solid site so do not update density but set velocity to zero
					rho(i,j,k,M_lim,K_lim) = 1.0;
					u(i,j,k,0,M_lim,K_lim,dims) = 0.0;
					u(i,j,k,1,M_lim,K_lim,dims) = 0.0;
#if (dims == 3)
					u(i,j,k,2,M_lim,K_lim,dims) = 0.0;
#endif


				} else {

					// Any other of type of site compute both density and velocity from populations
					rho_temp = 0.0; fux_temp = 0.0; fuy_temp = 0.0; fuz_temp = 0.0;

					for (int v = 0; v < nVels; v++) {

						// Sum up to find mass flux
						fux_temp += (double)c[0][v] * f(i,j,k,v,M_lim,K_lim,nVels);
						fuy_temp += (double)c[1][v] * f(i,j,k,v,M_lim,K_lim,nVels);
						fuz_temp += (double)c[2][v] * f(i,j,k,v,M_lim,K_lim,nVels);

						// Sum up to find density
						rho_temp += f(i,j,k,v,M_lim,K_lim,nVels);

					}

					// Assign density
					rho(i,j,k,M_lim,K_lim) = rho_temp;

					// Add forces to momentum (rho * time step * 0.5 * force -- eqn 19 in Favier 2014)
					fux_temp += rho_temp * (1 / pow(2,level)) * 0.5 * force_xyz(i,j,k,0,M_lim,K_lim,dims);
					fuy_temp += rho_temp * (1 / pow(2,level)) * 0.5 * force_xyz(i,j,k,1,M_lim,K_lim,dims);
#if (dims == 3)
					fuz_temp += rho_temp * (1 / pow(2,level)) * 0.5 * force_xyz(i,j,k,2,M_lim,K_lim,dims);
#endif

					// Assign velocity
					u(i,j,k,0,M_lim,K_lim,dims) = fux_temp / rho_temp;
					u(i,j,k,1,M_lim,K_lim,dims) = fuy_temp / rho_temp;
#if (dims == 3)
					u(i,j,k,2,M_lim,K_lim,dims) = fuz_temp / rho_temp;
#endif

				}

				// Update time-averaged quantities thus...

				// Multiply current value by completed time steps to get sum
				ta_temp = rho_timeav(i,j,k,M_lim,K_lim) * (double)t;
				// Add new value
				ta_temp += rho(i,j,k,M_lim,K_lim);
				// Divide by completed time steps + 1 to get new average
				rho_timeav(i,j,k,M_lim,K_lim) = ta_temp / (double)(t+1);

				// Repeat for other quantities
				int ta_count = 0;
				for (int p = 0; p < dims; p++) {
					ta_temp = ui_timeav(i,j,k,p,M_lim,K_lim,dims) * (double)t;
					ta_temp += u(i,j,k,p,M_lim,K_lim,dims);
					ui_timeav(i,j,k,p,M_lim,K_lim,dims) = ta_temp / (double)(t+1);
					// Do necessary products
					for (int q = p; q < dims; q++) {
						ta_temp = uiuj_timeav(i,j,k,ta_count,M_lim,K_lim,(3*dims-3)) * (double)t;
						ta_temp += ( u(i,j,k,p,M_lim,K_lim,dims) * u(i,j,k,q,M_lim,K_lim,dims) );
						uiuj_timeav(i,j,k,ta_count,M_lim,K_lim,(3*dims-3)) = ta_temp / (double)(t+1);
						ta_count++;
					}
				}



			}
		}
	}

}


// ***************************************************************************************************
// Overload of macroscopic quantity calculation to allow it to be applied to a single site as used by
// the MPI unpacking routine to update the values for the next collision step. This routine does not
// update the time-averaged quantities.
void GridObj::LBM_macro( int i, int j, int k ) {

	// Declarations
	int N_lim = XPos.size();
	int M_lim = YPos.size();
	int K_lim = ZPos.size();
	double rho_temp = 0.0;
	double fux_temp = 0.0;
	double fuy_temp = 0.0;
	double fuz_temp = 0.0;


	if (LatTyp(i,j,k,M_lim,K_lim) == 2) {

		// Refined site so set both density and velocity to zero
		rho(i,j,k,M_lim,K_lim) = 0.0;
		u(i,j,k,0,M_lim,K_lim,dims) = 0.0;
		u(i,j,k,1,M_lim,K_lim,dims) = 0.0;
#if (dims == 3)
		u(i,j,k,2,M_lim,K_lim,dims) = 0.0;
#endif

	} else if (LatTyp(i,j,k,M_lim,K_lim) == 0 || LatTyp(i,j,k,M_lim,K_lim) == 5) {

		// Solid site so do not update density but set velocity to zero
		rho(i,j,k,M_lim,K_lim) = 1.0;
		u(i,j,k,0,M_lim,K_lim,dims) = 0.0;
		u(i,j,k,1,M_lim,K_lim,dims) = 0.0;
#if (dims == 3)
		u(i,j,k,2,M_lim,K_lim,dims) = 0.0;
#endif

	} else {

		// Any other of type of site compute both density and velocity from populations
		rho_temp = 0.0; fux_temp = 0.0; fuy_temp = 0.0; fuz_temp = 0.0;

		for (int v = 0; v < nVels; v++) {

			// Sum up to find mass flux
			fux_temp += (double)c[0][v] * f(i,j,k,v,M_lim,K_lim,nVels);
			fuy_temp += (double)c[1][v] * f(i,j,k,v,M_lim,K_lim,nVels);
			fuz_temp += (double)c[2][v] * f(i,j,k,v,M_lim,K_lim,nVels);

			// Sum up to find density
			rho_temp += f(i,j,k,v,M_lim,K_lim,nVels);

		}

		// Assign density
		rho(i,j,k,M_lim,K_lim) = rho_temp;

		// Add forces to momentum (rho * time step * 0.5 * force -- eqn 19 in Favier 2014)
		fux_temp += rho_temp * (1 / pow(2,level)) * 0.5 * force_xyz(i,j,k,0,M_lim,K_lim,dims);
		fuy_temp += rho_temp * (1 / pow(2,level)) * 0.5 * force_xyz(i,j,k,1,M_lim,K_lim,dims);
#if (dims == 3)
		fuz_temp += rho_temp * (1 / pow(2,level)) * 0.5 * force_xyz(i,j,k,2,M_lim,K_lim,dims);
#endif

		// Assign velocity
		u(i,j,k,0,M_lim,K_lim,dims) = fux_temp / rho_temp;
		u(i,j,k,1,M_lim,K_lim,dims) = fuy_temp / rho_temp;
#if (dims == 3)
		u(i,j,k,2,M_lim,K_lim,dims) = fuz_temp / rho_temp;
#endif

	}

}


// ***************************************************************************************************
// Explosion operation
void GridObj::LBM_explode( int RegionNumber ) {

	// Declarations
	GridObj* fGrid = NULL;
	GridUtils::getGrid(MpiManager::Grids,level+1,RegionNumber,fGrid);
	int y_start, x_start, z_start;
	int M_fine = fGrid->YPos.size();
	int M_coarse = YPos.size();
	int K_coarse = ZPos.size();
	int K_fine = fGrid->ZPos.size();

	// Loop over coarse grid (just region of interest)
	for (size_t i = fGrid->CoarseLimsX[0]; i <= fGrid->CoarseLimsX[1]; i++) {
		for (size_t j = fGrid->CoarseLimsY[0]; j <= fGrid->CoarseLimsY[1]; j++) {
			for (size_t k = fGrid->CoarseLimsZ[0]; k <= fGrid->CoarseLimsZ[1]; k++) {

				// If TL to lower level and point belongs to region then explosion required
				if (LatTyp(i,j,k,M_coarse,K_coarse) == 4) {

					// Lookup indices for lower level
					x_start = fGrid->CoarseLimsX[0];
					y_start = fGrid->CoarseLimsY[0];
					z_start = fGrid->CoarseLimsZ[0];

					// Find indices of fine site
					vector<int> idx_fine = GridUtils::getFineIndices(i, x_start, j, y_start, k, z_start);
					int fi = idx_fine[0];
					int fj = idx_fine[1];
					int fk = idx_fine[2];

					// Only pass information to lower levels if lower level is expecting it 
					// (i.e. not a boundary site)
					if (fGrid->LatTyp(fi,fj,fk,M_fine,K_fine) == 3) {

						// Update fine grid values according to Rohde et al.
						for (int v = 0; v < nVels; v++) {

							// Get coarse site value
							double coarse_f = f(i,j,k,v,M_coarse,K_coarse,nVels);

#if (dims == 3)
							// 3D Case -- cube of 8 cells

							// Copy coarse to fine
							fGrid->f(fi,	fj,		fk,		v,M_fine,K_fine,nVels)	= coarse_f;
							fGrid->f(fi+1,	fj,		fk,		v,M_fine,K_fine,nVels)	= coarse_f;
							fGrid->f(fi,	fj+1,	fk,		v,M_fine,K_fine,nVels)	= coarse_f;
							fGrid->f(fi+1,	fj+1,	fk,		v,M_fine,K_fine,nVels)	= coarse_f;
							fGrid->f(fi,	fj,		fk+1,	v,M_fine,K_fine,nVels)	= coarse_f;
							fGrid->f(fi+1,	fj,		fk+1,	v,M_fine,K_fine,nVels)	= coarse_f;
							fGrid->f(fi,	fj+1,	fk+1,	v,M_fine,K_fine,nVels)	= coarse_f;
							fGrid->f(fi+1,	fj+1,	fk+1,	v,M_fine,K_fine,nVels)	= coarse_f;

#else

							// 2D Case -- square of 4 cells

							// Copy coarse to fine
							fGrid->f(fi,	fj,		v,M_fine,nVels)		= coarse_f;
							fGrid->f(fi+1,	fj,		v,M_fine,nVels)		= coarse_f;
							fGrid->f(fi,	fj+1,	v,M_fine,nVels)		= coarse_f;
							fGrid->f(fi+1,	fj+1,	v,M_fine,nVels)		= coarse_f;

#endif
						}
					}

				}

			}
		}
	}


}


// ***************************************************************************************************
// Coalesce operation -- called from coarse level
void GridObj::LBM_coalesce( int RegionNumber ) {

	// Declarations
	GridObj* fGrid;
	GridUtils::getGrid(MpiManager::Grids,level+1,RegionNumber,fGrid);
	int y_start, x_start, z_start;
	int M_fine = fGrid->YPos.size();
	int M_coarse = YPos.size();
	int K_coarse = ZPos.size();
#if (dims == 3)
	int K_fine = fGrid->ZPos.size();
#else
	int K_fine = 0;
#endif


	// Loop over coarse grid (only region of interest)
	for (size_t i = fGrid->CoarseLimsX[0]; i <= fGrid->CoarseLimsX[1]; i++) {
		for (size_t j = fGrid->CoarseLimsY[0]; j <= fGrid->CoarseLimsY[1]; j++) {
			for (size_t k = fGrid->CoarseLimsZ[0]; k <= fGrid->CoarseLimsZ[1]; k++) {

				// If TL to lower level then fetch values from lower level
				if (LatTyp(i,j,k,M_coarse,K_coarse) == 4 || LatTyp(i,j,k,M_coarse,K_coarse) == 5) {

					// Lookup indices for lower level
					x_start = fGrid->CoarseLimsX[0];
					y_start = fGrid->CoarseLimsY[0];
					z_start = fGrid->CoarseLimsZ[0];

					// Find indices of fine site
					vector<int> idx_fine = GridUtils::getFineIndices(i, x_start, j, y_start, k, z_start);
					int fi = idx_fine[0];
					int fj = idx_fine[1];
					int fk = idx_fine[2];

					// Loop over directions
					for (int v = 0; v < nVels; v++) {

						// Check to see if f value is missing on coarse level
						if (f(i,j,k,v,M_coarse,K_coarse,nVels) == 0) {

#if (dims == 3)
							// 3D Case -- cube of 8 cells

							// Average the values
							f(i,j,k,v,M_coarse,K_coarse,nVels) = (
								fGrid->f(fi,	fj,		fk,		v,M_fine,K_fine,nVels) +
								fGrid->f(fi+1,	fj,		fk,		v,M_fine,K_fine,nVels) +
								fGrid->f(fi,	fj+1,	fk,		v,M_fine,K_fine,nVels) +
								fGrid->f(fi+1,	fj+1,	fk,		v,M_fine,K_fine,nVels) +
								fGrid->f(fi,	fj,		fk+1,	v,M_fine,K_fine,nVels) +
								fGrid->f(fi+1,	fj,		fk+1,	v,M_fine,K_fine,nVels) +
								fGrid->f(fi,	fj+1,	fk+1,	v,M_fine,K_fine,nVels) +
								fGrid->f(fi+1,	fj+1,	fk+1,	v,M_fine,K_fine,nVels)
								) / pow(2, dims);

#else

							// 2D Case -- square of 4 cells

							// Average the values
							f(i,j,k,v,M_coarse,K_coarse,nVels) = (
								fGrid->f(fi,	fj,		v,M_fine,nVels) +
								fGrid->f(fi+1,	fj,		v,M_fine,nVels) +
								fGrid->f(fi,	fj+1,	v,M_fine,nVels) +
								fGrid->f(fi+1,	fj+1,	v,M_fine,nVels)
								) / pow(2, dims);

#endif
						}

					}

				}

			}
		}
	}

}

// ***************************************************************************************************
