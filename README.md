# Wireless-Sensor-Network-Simulation
MPI + thread program to represent nodes in a wireless sensor network

The deployed wireless sensor network comprises m × n nodes in a cartesian grid, and a base station. In the
example illustration in Figure 1, m = 3 and n = 3. However, the values of m and n vary as specified by the user
during program runtime. Each node in this cartesian grid simulates the behaviour of the ground sensor, and
each node communicates with its immediate adjacent nodes. These nodes can exchange data through unicast
and broadcast modes of communications.

Each node in the WSN can independently exchange data with the base-station. Base-station for this WSN is an
additional computer node that is entrusted with the task of gathering data from all sensor nodes in the WSN. All
communications among nodes, and between the nodes and the base station are achieved using Message
Passing Interface (MPI). In essence, each node and base station can be represented as a MPI process.

A fault detection mechanism is simulated whereby the sudden failure of a node is detected by the base
station and/or adjacent sensor nodes. Once a faulty node is detected, a method is implemented to notify the base station to log the fault and
to gracefully shutdown the entire network.
