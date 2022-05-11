// Copyright 2022 daohu527@gmail.com
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
//
// This is an implementation of the algorithm described in the following papers:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.
//   T. Shan and B. Englot. LeGO-LOAM: Lightweight and Ground-Optimized Lidar Odometry and Mapping on Variable Terrain
//      IEEE/RSJ International Conference on Intelligent Robots and Systems (IROS). October 2018.


//  Created Date: 2022-5-5
//  Author: daohu527


#include "modules/tools/ilego_loam/include/utility.h"
#include "modules/tools/ilego_loam/include/image_projection.h"


namespace apollo {
namespace tools {

static constexpr int8_t dirs[4][2] = {{-1, 0}, {0, 1}, {0, -1}, {1, 0}};
static constexpr int LABEL_INIT = 0;
static constexpr int LABEL_INVALID = 999999;

ImageProjection::ImageProjection() :
    nan_point(quiet_NaN(), quiet_NaN(), quiet_NaN(), -1),
    all_pushed_indX(N_SCAN * HORIZON_SCAN),
    all_pushed_indY(N_SCAN * HORIZON_SCAN),
    queue_indX(N_SCAN * HORIZON_SCAN),
    queue_indY(N_SCAN * HORIZON_SCAN) {}

ImageProjection::~ImageProjection() {}

bool ImageProjection::Init() {
  sub_laser_cloud = node_->CreateReader<drivers::PointCloud>(
      FLAGS_lidar_topic,
      [&](const DriverPointCloudPtr& point_cloud){
        CloudHandler(point_cloud);
      });

  pub_full_cloud = node_->CreateWriter<drivers::PointCloud>("/full_cloud_projected");
  pub_full_info_cloud = node_->CreateWriter<drivers::PointCloud>("/full_cloud_info");

  pub_ground_cloud = node_->CreateWriter<drivers::PointCloud>("/ground_cloud");
  pub_segmented_cloud = node_->CreateWriter<drivers::PointCloud>("/segmented_cloud");
  pub_segmented_cloud_pure = node_->CreateWriter<drivers::PointCloud>("/segmented_cloud_pure");
  pub_segmented_cloud_info = node_->CreateWriter<cloud_msgs::CloudInfo>("/segmented_cloud_info");
  pub_outlier_cloud = node_->CreateWriter<drivers::PointCloud>("/outlier_cloud");
}

void ImageProjection::AllocateMemory() {
  laser_cloud_in = std::make_shared<pcl::PointCloud<PointType>>();
  laser_cloud_in_ring = std::make_shared<pcl::PointCloud<PointXYZIR>>();

  full_cloud = std::make_shared<pcl::PointCloud<PointType>>();
  full_info_cloud = std::make_shared<pcl::PointCloud<PointType>>();
  full_cloud->points.resize(N_SCAN * HORIZON_SCAN);
  full_info_cloud->points.resize(N_SCAN * HORIZON_SCAN);

  ground_cloud = std::make_shared<pcl::PointCloud<PointType>>();

  segmented_cloud = std::make_shared<pcl::PointCloud<PointType>>();
  segmented_cloud_pure = std::make_shared<pcl::PointCloud<PointType>>();
  outlier_cloud = std::make_shared<pcl::PointCloud<PointType>>();

  // todo(zero): need to set default value
  seg_msg.startRingIndex->Reserve(N_SCAN);
  seg_msg.endRingIndex->Reserve(N_SCAN);

  seg_msg.segmentedCloudGroundFlag->Reserve(N_SCAN * HORIZON_SCAN);
  seg_msg.segmentedCloudColInd->Reserve(N_SCAN * HORIZON_SCAN);
  seg_msg.segmentedCloudRange->Reserve(N_SCAN * HORIZON_SCAN);
}

void ImageProjection::ResetParameters() {
  laser_cloud_in->clear();
  laser_cloud_in_ring->clear();
  ground_cloud->clear();
  segmented_cloud->clear();
  segmented_cloud_pure->clear();
  outlier_cloud->clear();

  range_mat = cv::Mat(N_SCAN, HORIZON_SCAN, CV_32F, cv::Scalar::all(FLT_MAX));
  ground_mat = cv::Mat(N_SCAN, HORIZON_SCAN, CV_8S, cv::Scalar::all(0));
  label_mat = cv::Mat(N_SCAN, HORIZON_SCAN, CV_32S, cv::Scalar::all(LABEL_INIT));
  label_count = 1;

  std::fill(full_cloud->points.begin(), full_cloud->points.end(), nan_point);
  std::fill(full_info_cloud->points.begin(), full_info_cloud->points.end(), nan_point);
}

void ImageProjection::CopyPointCloud(const DriverPointCloudPtr& laser_cloud_msg) {
  cloud_header = laser_cloud_msg->header;

  ToPclPointCloud(laser_cloud_msg, laser_cloud_in);

  std::vector<int> indices;
  pcl::removeNaNFromPointCloud(*laser_cloud_in, *laser_cloud_in, indices);

  if (FLAGS_use_cloud_ring) {
    ToPclPointCloud(laser_cloud_msg, laser_cloud_in_ring);
    ACHECK(laser_cloud_in_ring->is_dense) <<
        "Point cloud is not in dense format, please remove NaN points first!";
  }
}

void ImageProjection::FindStartEndAngle() {
  seg_msg.startOrientation = atan2(laser_cloud_in->points.front().x, laser_cloud_in->points.front().y);
  seg_msg.endOrientation = atan2(laser_cloud_in->points.back().x, laser_cloud_in->points.back().y) + 2 * M_PI;

  seg_msg.orientationDiff = seg_msg.endOrientation - seg_msg.startOrientation;
  CHECK(seg_msg.orientationDiff < 3 * M_PI) << "Point cloud orientation diff >= 3*M_PI";
  CHECK(seg_msg.orientationDiff > M_PI) << "Point cloud orientation diff <= M_PI";
}

void ImageProjection::ProjectPointCloud() {
  for (size_t i = 0; i < laser_cloud_in->points.size(); ++i) {
    PointType this_point = laser_cloud_in->points[i];

    size_t row_idn;
    if (FLAGS_use_cloud_ring) {
      row_idn = laser_cloud_in_ring->points[i].ring;
    } else {
      float vertical_angle = atan2(this_point.z, sqrt(this_point.x*this_point.x + this_point.y*this_point.y)) * 180 / M_PI;
      row_idn = (vertical_angle + ang_bottom) / ang_res_y;
    }

    if (row_idn >= N_SCAN)
      continue;

    float horizon_angle = atan2(this_point.x, this_point.y) * 180 / M_PI;

    size_t column_idn = -round((horizon_angle - 90) / ang_res_x) + HORIZON_SCAN / 2;
    if (column_idn >= HORIZON_SCAN)
      column_idn -= HORIZON_SCAN;

    if (column_idn >= HORIZON_SCAN)
      continue;

    float range = sqrt(this_point.x*this_point.x + this_point.y*this_point.y + this_point.z*this_point.z);
    if (range < FLAGS_sensor_minimum_range)
      continue;

    range_mat.at<float>(row_idn, column_idn) = range;
    this_point.intensity = static_cast<float>(row_idn) +
        static_cast<float>(column_idn) / 10000;

    size_t index = column_idn + row_idn * HORIZON_SCAN;

    full_cloud->points[index] = this_point;
    full_info_cloud->points[index] = this_point;
    full_info_cloud->points[index].intensity = range;
  }
}

void ImageProjection::GroundRemoval() {
  for (size_t i = 0; i < groundScanInd; ++i) {
    for (size_t j = 0; j < HORIZON_SCAN; ++j) {
      size_t lower_ind = j + i * HORIZON_SCAN;
      size_t upper_ind = j + (i + 1) * HORIZON_SCAN;

      if (IsNaN(full_cloud->points[lower_ind]) ||
          IsNaN(full_cloud->points[upper_ind])) {
        ground_mat.at<int8_t>(i, j) = -1;
        continue;
      }

      float diffx = full_cloud->points[upper_ind].x - full_cloud->points[lower_ind].x;
      float diffy = full_cloud->points[upper_ind].y - full_cloud->points[lower_ind].y;
      float diffz = full_cloud->points[upper_ind].z - full_cloud->points[lower_ind].z;

      float angle = atan2(diffz, sqrt(diffx*diffx + diffy*diffy)) * 180 / M_PI;

      if (fabs(angle - FLAGS_sensor_mount_angle) <= 10) {
        ground_mat.at<int8_t>(i, j) = 1;
        ground_mat.at<int8_t>(i + 1, j) = 1;
      }
    }
  }

  for (size_t i = 0; i < N_SCAN; ++i) {
    for (size_t j = 0; j < HORIZON_SCAN; ++j) {
      if (ground_mat.at<int8_t>(i, j) == 1 || range_mat.at<float>(i, j) == FLT_MAX) {
        label_mat.at<int>(i, j) = -1;
      }
    }
  }

  for (size_t i = 0; i <= groundScanInd; ++i) {
    for (size_t j = 0; j < HORIZON_SCAN; ++j) {
      if (ground_mat.at<int8_t>(i, j) == 1)
        ground_cloud->push_back(full_cloud->points[j + i * HORIZON_SCAN]);
    }
  }
}

void ImageProjection::CloudSegmentation() {
  for (size_t i = 0; i < N_SCAN; ++i) {
    for (size_t j = 0; j < HORIZON_SCAN; ++j) {
      if (label_mat.at<int>(i, j) == LABEL_INIT)
        LabelComponents(i, j);
    }
  }

  int size_of_seg_cloud = 0;
  for (size_t i = 0; i < N_SCAN; ++i) {
    seg_msg.startRingIndex[i] = size_of_seg_cloud - 1 + 5;

    for (size_t j = 0; j < HORIZON_SCAN; ++j) {
      if (label_mat.at<int>(i, j) > 0 || ground_mat.at<int8_t>(i, j) == 1) {
        if (label_mat.at<int>(i, j) == LABEL_INVALID) {
          if (i > groundScanInd && j % 5 == 0) {
            outlier_cloud->push_back(full_cloud->points[j + i * Horizon_SCAN]);
          }
          continue;
        }

        if (ground_mat.at<int8_t>(i, j) == 1) {
          if (j%5 != 0 && j > 5 && j < HORIZON_SCAN-5)
            continue;
        }

        seg_msg.segmentedCloudGroundFlag[size_of_seg_cloud] = (ground_mat.at<int8_t>(i, j) == 1);
        seg_msg.segmentedCloudColInd[size_of_seg_cloud] = j;
        seg_msg.segmentedCloudRange[size_of_seg_cloud] = range_mat.at<float>(i, j);
        segmented_cloud->push_back(full_cloud->points[j + i * Horizon_SCAN]);
        ++size_of_seg_cloud;
      }
    }
    seg_msg.endRingIndex[i] = size_of_seg_cloud - 1 - 5;
  }

  for (size_t i = 0; i < N_SCAN; ++i) {
    for (size_t j = 0; j < HORIZON_SCAN; ++j) {
      if (label_mat.at<int>(i, j) > 0 && label_mat.at<int>(i, j) != LABEL_INVALID) {
        segmented_cloud_pure->push_back(full_cloud->points[j + i*Horizon_SCAN]);
        segmented_cloud_pure->points.back().intensity = label_mat.at<int>(i, j);
      }
    }
  }
}

void ImageProjection::PublishCloud() {
  // todo(zero): check the header
  seg_msg.header.stamp = cloud_header.timestamp_sec;
  pub_segmented_cloud_info->Write(seg_msg);

  drivers::PointCloud laser_cloud_temp;
  ToDriverPointCloud(outlier_cloud, laser_cloud_temp);
  laser_cloud_temp.header.timestamp_sec = cloud_header.timestamp_sec;
  laser_cloud_temp.header.frame_id = "base_link";
  pub_outlier_cloud->Write(laser_cloud_temp);

  laser_cloud_temp.point.Clear();
  ToDriverPointCloud(segmented_cloud, laser_cloud_temp);
  laser_cloud_temp.header.timestamp_sec = cloud_header.timestamp_sec;
  laser_cloud_temp.header.frame_id = "base_link";
  pub_segmented_cloud->Write(laser_cloud_temp);

  laser_cloud_temp.point.Clear();
  ToDriverPointCloud(full_cloud, laser_cloud_temp);
  laser_cloud_temp.header.timestamp_sec = cloud_header.timestamp_sec;
  laser_cloud_temp.header.frame_id = "base_link";
  pub_full_cloud->Write(laser_cloud_temp);

  laser_cloud_temp.point.Clear();
  ToDriverPointCloud(full_info_cloud, laser_cloud_temp);
  laser_cloud_temp.header.timestamp_sec = cloud_header.timestamp_sec;
  laser_cloud_temp.header.frame_id = "base_link";
  pub_full_info_cloud->Write(laser_cloud_temp);

  laser_cloud_temp.point.Clear();
  ToDriverPointCloud(ground_cloud, laser_cloud_temp);
  laser_cloud_temp.header.timestamp_sec = cloud_header.timestamp_sec;
  laser_cloud_temp.header.frame_id = "base_link";
  pub_ground_cloud->Write(laser_cloud_temp);

  laser_cloud_temp.point.Clear();
  ToDriverPointCloud(segmented_cloud_pure, laser_cloud_temp);
  laser_cloud_temp.header.timestamp_sec = cloud_header.timestamp_sec;
  laser_cloud_temp.header.frame_id = "base_link";
  pub_segmented_cloud_pure->Write(laser_cloud_temp);
}

void ImageProjection::LabelComponents(int row, int col) {
  queue_indX[0] = row;
  queue_indY[0] = col;

  all_pushed_indX[0] = row;
  all_pushed_indY[0] = col;
  int all_pushed_ind_size = 1;

  int queue_start_ind = 0;
  int queue_end_ind = 1;
  bool line_count_flag[N_SCAN] = {false};

  int queue_size = 1;
  while(queue_size > 0) {
    int from_indX = queue_indX[queue_start_ind];
    int from_indY = queue_indY[queue_start_ind];
    --queue_size;
    ++queue_start_ind;

    label_mat.at<int>(from_indX, from_indY) = label_count;
    for (int i = 0; i < 4; ++i) {
      int this_indX = from_indX + dirs[i][0];
      int this_indY = from_indY + dirs[i][1];

      if (this_indX < 0 || this_indX >= N_SCAN)
        continue;

      if (this_indY < 0)
        this_indY = HORIZON_SCAN - 1;
      if (this_indY >= HORIZON_SCAN)
        this_indY = 0;

      if (label_mat.at<int>(this_indX, this_indY) != LABEL_INIT)
        continue;

      auto [d2, d1] = std::minmax(range_mat.at<float>(from_indX, from_indY),
          range_mat.at<float>(this_indX, this_indY));

      if (dirs[i][0] == 0)
        alpha = segmentAlphaX;
      else
        alpha = segmentAlphaY;

      float angle = atan2(d2 * sin(alpha), (d1 - d2 * cos(alpha)));

      if (angle > segmentTheta) {
        queue_indX[queue_end_ind] = this_indX;
        queue_indY[queue_end_ind] = this_indY;
        ++queue_size;
        ++queue_end_ind;

        label_mat.at<int>(this_indX, this_indY) = label_count;
        line_count_flag[this_indX] = true;

        all_pushed_indX[all_pushed_ind_size] = this_indX;
        all_pushed_indY[all_pushed_ind_size] = this_indY;
        ++all_pushed_ind_size;
      }
    }
  }

  bool feasible_segment = false;
  if (all_pushed_ind_size >= 30) {
    feasible_segment = true;
  } else if (all_pushed_ind_size >= segmentValidPointNum) {
    int line_count = 0;
    for (size_t i = 0; i < N_SCAN; ++i) {
      if (line_count_flag[i]) {
        ++line_count;
      }
    }

    if (line_count >= segmentValidLineNum)
      feasible_segment = true;
  }

  if (feasible_segment) {
    ++label_count;
  } else {
    for (size_t i = 0; i < all_pushed_ind_size; ++i) {
      label_mat.at<int>(all_pushed_indX[i], all_pushed_indY[i]) = LABEL_INVALID;
    }
  }
}

void ImageProjection::CloudHandler(const DriverPointCloudPtr& laser_cloud_msg) {
  // 1. convert ros message to pcl point cloud
  CopyPointCloud(laser_cloud_msg);
  // 2. start and end angle of a scan
  FindStartEndAngle();
  // 3. range image projection
  ProjectPointCloud();
  // 4. mark ground points
  GroundRemoval();
  // 5. point cloud segmentation
  CloudSegmentation();
  // 6. publish all clouds
  PublishCloud();
  // 7. reset parameters for next iteration
  ResetParameters();
}


}  // namespace tools
}  // namespace apollo
