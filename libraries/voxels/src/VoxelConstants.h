//
//  VoxelConstants.h
//  hifi
//
//  Created by Brad Hefta-Gaub on 4/29/13.
//
//
//  Various important constants used throughout the system related to voxels
//
//  

#ifndef __hifi_VoxelConstants_h__
#define __hifi_VoxelConstants_h__

const int MAX_VOXEL_PACKET_SIZE = 1492;
const int MAX_TREE_SLICE_BYTES = 26;
const int TREE_SCALE = 10;
const int MAX_VOXELS_PER_SYSTEM = 250000;
const int VERTICES_PER_VOXEL = 24;
const int VERTEX_POINTS_PER_VOXEL = 3 * VERTICES_PER_VOXEL;
const int INDICES_PER_VOXEL = 3 * 12;
const int COLOR_VALUES_PER_VOXEL = 3 * VERTICES_PER_VOXEL;

#endif