// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All right reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Radu Serban
// =============================================================================
//
// Black-box program for using an external optimization program for tuning 
// parameters of a PID steering controller.
//
// =============================================================================

#include <vector>
#include <valarray>
#include <iostream>
#include <sstream>
#include <fstream>

#include "chrono/core/ChRealtimeStep.h"
#include "chrono/geometry/ChCLineBezier.h"
#include "chrono/assets/ChLineShape.h"
#include "chrono/utils/ChUtilsInputOutput.h"

#include "chrono_vehicle/ChVehicleModelData.h"
#include "chrono_vehicle/vehicle/Vehicle.h"
#include "chrono_vehicle/powertrain/SimplePowertrain.h"
#include "chrono_vehicle/tire/RigidTire.h"
#include "chrono_vehicle/tire/LugreTire.h"
#include "chrono_vehicle/terrain/RigidTerrain.h"

#include "chrono_vehicle/ChDriver.h"
#include "chrono_vehicle/utils/ChSteeringController.h"

using namespace chrono;
using namespace geometry;

// =============================================================================
// Global definitions

typedef std::valarray<double> DataArray;

struct Data {
    Data(int n) {
        time.resize(n);
        err_x.resize(n);
        err_y.resize(n);
        err_z.resize(n);
    }

    DataArray time;   // current time
    DataArray err_x;  // x component of vehicle location error
    DataArray err_y;  // y component of vehicle location error
    DataArray err_z;  // z component of vehicle location error
};

enum TireModelType {
    RIGID,
    PACEJKA,
    LUGRE,
    FIALA
};

// Type of tire model
TireModelType tire_model = RIGID;

// Input file names for the path-follower driver model
std::string controller_file("generic/driver/SteeringController.json");
std::string path_file("pathS.txt");

// Output file name
std::string out_file("results.out");

// JSON file names for vehicle model, tire models, and (simple) powertrain
std::string vehicle_file("generic/vehicle/Vehicle_DoubleWishbones.json");
std::string rigidtire_file("generic/tire/RigidTire.json");
std::string lugretire_file("generic/tire/LugreTire.json");
std::string simplepowertrain_file("generic/powertrain/SimplePowertrain.json");

// Initial vehicle position and orientation
ChVector<> initLoc(-125, -125, 0.6);
ChQuaternion<> initRot(1, 0, 0, 0);

// Rigid terrain dimensions
double terrainHeight = 0;
double terrainLength = 300.0;  // size in X direction
double terrainWidth = 300.0;   // size in Y direction

// Simulation step size and simulation length
double step_size = 2e-3;        // integration step size
int num_steps_settling = 3000;  // number of steps for settling
int num_steps = 5000;           // number of steps for data colection

// =============================================================================
// Forward declarations

void processData(const utils::CSV_writer& csv, const Data& data);

// =============================================================================
// Definition of custom driver with PID steering controller

class MyDriver : public ChDriver {
  public:
    MyDriver(chrono::ChVehicle& vehicle, const std::string& filename, chrono::ChBezierCurve* path)
        : m_vehicle(vehicle), m_PID(filename, path) {
        m_PID.Reset(vehicle);
    }

    ~MyDriver() {}

    chrono::ChPathSteeringController& GetSteeringController() { return m_PID; }

    void Reset() { m_PID.Reset(m_vehicle); }

    virtual void Advance(double step) override {
        m_throttle = 0.12;
        m_braking = 0;
        SetSteering(m_PID.Advance(m_vehicle, step), -1, 1);
    }

  private:
    chrono::ChVehicle& m_vehicle;
    chrono::ChPathSteeringController m_PID;
};

// =============================================================================
// Main driver program

int main(int argc, char* argv[]) {
    // Create and initialize the vehicle system
    Vehicle vehicle(vehicle::GetDataFile(vehicle_file));
    vehicle.Initialize(ChCoordsys<>(initLoc, initRot));

    // Create the terrain
    RigidTerrain terrain(vehicle.GetSystem(), terrainHeight, terrainLength, terrainWidth, 0.9);

    // Create and initialize the powertrain system
    SimplePowertrain powertrain(vehicle::GetDataFile(simplepowertrain_file));
    powertrain.Initialize();

    // Create and initialize the tires
    int num_axles = vehicle.GetNumberAxles();
    int num_wheels = 2 * num_axles;

    std::vector<ChSharedPtr<ChTire> > tires(num_wheels);

    switch (tire_model) {
        case RIGID: {
            std::vector<ChSharedPtr<RigidTire> > tires_rigid(num_wheels);
            for (int i = 0; i < num_wheels; i++) {
                tires_rigid[i] = ChSharedPtr<RigidTire>(new RigidTire(vehicle::GetDataFile(rigidtire_file), terrain));
                tires_rigid[i]->Initialize(vehicle.GetWheelBody(i));
                tires[i] = tires_rigid[i];
            }
            break;
        }
        case LUGRE: {
            std::vector<ChSharedPtr<LugreTire> > tires_lugre(num_wheels);
            for (int i = 0; i < num_wheels; i++) {
                tires_lugre[i] = ChSharedPtr<LugreTire>(new LugreTire(vehicle::GetDataFile(lugretire_file), terrain));
                tires_lugre[i]->Initialize(vehicle.GetWheelBody(i));
                tires[i] = tires_lugre[i];
            }
            break;
        }
    }

    // Create the driver system
    ChBezierCurve* path = ChBezierCurve::read(vehicle::GetDataFile(path_file));
    MyDriver driver(vehicle, vehicle::GetDataFile(controller_file), path);
    driver.Reset();

    // Create a path tracker to keep track of the error in vehicle location.
    ChBezierCurveTracker tracker(path);

    // ---------------
    // Simulation loop
    // ---------------

    // Initialize data collectors
    utils::CSV_writer csv("\t");
    csv.stream().setf(std::ios::scientific | std::ios::showpos);
    csv.stream().precision(6);

    Data data(num_steps);

    // Inter-module communication data
    ChTireForces tire_forces(num_wheels);
    ChWheelStates wheel_states(num_wheels);
    double driveshaft_speed;
    double powertrain_torque;
    double throttle_input;
    double steering_input;
    double braking_input;

    for (int it = 0; it < num_steps_settling + num_steps; it++) {
        bool settling = (it < num_steps_settling);

        // Collect data
        if (!settling) {
            const ChVector<> sentinel = driver.GetSteeringController().GetSentinelLocation();
            const ChVector<> target = driver.GetSteeringController().GetTargetLocation();
            const ChVector<> vehicle_location = vehicle.GetChassisPos();
            ChVector<> vehicle_target;
            tracker.calcClosestPoint(vehicle_location, vehicle_target);
            ChVector<> vehicle_err = vehicle_target - vehicle_location;

            csv << vehicle.GetChTime() << vehicle_location << vehicle_target << vehicle_err << std::endl;

            int id = it - num_steps_settling;
            data.time[id] = vehicle.GetChTime();
            data.err_x[id] = vehicle_err.x;
            data.err_y[id] = vehicle_err.y;
            data.err_z[id] = vehicle_err.z;
        }

        // Collect output data from modules (for inter-module communication)
        if (settling) {
            throttle_input = 0;
            steering_input = 0;
            braking_input = 0;
        } else {
            throttle_input = driver.GetThrottle();
            steering_input = driver.GetSteering();
            braking_input = driver.GetBraking();
        }
        powertrain_torque = powertrain.GetOutputTorque();
        driveshaft_speed = vehicle.GetDriveshaftSpeed();
        for (int i = 0; i < num_wheels; i++) {
            tire_forces[i] = tires[i]->GetTireForce();
            wheel_states[i] = vehicle.GetWheelState(i);
        }

        // Update modules (process inputs from other modules)
        double time = vehicle.GetChTime();
        driver.Update(time);
        powertrain.Update(time, throttle_input, driveshaft_speed);
        vehicle.Update(time, steering_input, braking_input, powertrain_torque, tire_forces);
        terrain.Update(time);
        for (int i = 0; i < num_wheels; i++)
            tires[i]->Update(time, wheel_states[i]);

        // Advance simulation for one timestep for all modules
        driver.Advance(step_size);
        powertrain.Advance(step_size);
        vehicle.Advance(step_size);
        terrain.Advance(step_size);
        for (int i = 0; i < num_wheels; i++)
            tires[i]->Advance(step_size);
    }

    processData(csv, data);

    return 0;
}

// =============================================================================
// Simulation data post-processing

void processData(const utils::CSV_writer& csv, const Data& data) {
    // Optionally, write simulation results to file for external post-processing
    csv.write_to_file(out_file);

    // Alternatively, post-process simulation results here and write out results
    DataArray err_norm2 = data.err_x * data.err_x + data.err_y * data.err_y + data.err_z * data.err_z;
    double L2_norm = std::sqrt(err_norm2.sum());
    double RMS_norm = std::sqrt(err_norm2.sum() / num_steps);
    double INF_norm = std::sqrt(err_norm2.max());

    std::cout << "|err|_L2 =  " << L2_norm << std::endl;
    std::cout << "|err|_RMS = " << RMS_norm << std::endl;
    std::cout << "|err|_INF = " << INF_norm << std::endl;

    ////std::ofstream ofile(out_file.c_str());
    ////ofile << L2_norm << std::endl;
    ////ofile.close();
}
