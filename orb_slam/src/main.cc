/**
* This file is part of ORB-SLAM.
*
* Copyright (C) 2014 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <http://webdiis.unizar.es/~raulmur/orbslam/>
*
* ORB-SLAM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM. If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <fstream>
#include <ros/ros.h>
#include <ros/package.h>
#include <boost/thread.hpp>
#include <boost/filesystem.hpp>

#include <opencv2/core/core.hpp>

#include "types/Map.h"
#include "types/KeyFrameDatabase.h"
#include "types/ORBVocabulary.h"

#include "threads/Tracking.h"
#include "threads/Relocalization.h"
#include "threads/MapMerging.h"
#include "threads/LocalMapping.h"
#include "threads/LoopClosing.h"

#include "publishers/FramePublisher.h"
#include "publishers/MapPublisher.h"

#include "util/Converter.h"
#include "util/FpsCounter.h"

#include <sstream>



using namespace std;


int main(int argc, char **argv)
{
    ros::init(argc, argv, "ORB_SLAM");
    ros::start();

    cout << endl << "ORB-SLAM Copyright (C) 2014 Raul Mur-Artal" << endl <<
            "This program comes with ABSOLUTELY NO WARRANTY;" << endl  <<
            "This is free software, and you are welcome to redistribute it" << endl <<
            "under certain conditions. See LICENSE.txt." << endl;

    if(argc != 3)
    {
        ROS_ERROR("Usage: rosrun ORB_SLAM ORB_SLAM path_to_vocabulary path_to_settings (absolute or relative to package directory)");
        ros::shutdown();
        return 1;
    }

    // Load Settings and Check
    string strSettingsFile = ros::package::getPath("orb_slam")+"/"+argv[2];

    cv::FileStorage fsSettings(strSettingsFile.c_str(), cv::FileStorage::READ);
    if(!fsSettings.isOpened())
    {
        ROS_ERROR("Wrong path to settings. Path must be absolute or relative to ORB_SLAM package directory.");
        ros::shutdown();
        return 1;
    }
    
    // Create fps counter
    FpsCounter fps_counter;

    //Create Frame Publisher for image_view
    ORB_SLAM::FramePublisher FramePub(&fps_counter);

    //Load ORB Vocabulary
    string strVocFile = ros::package::getPath("orb_slam")+"/"+argv[1];
    cout << endl << "Loading ORB Vocabulary. This could take a while." << endl;
    cv::FileStorage fsVoc(strVocFile.c_str(), cv::FileStorage::READ);
    if(!fsVoc.isOpened())
    {
        ROS_ERROR("Wrong path to vocabulary. Path must be absolute or relative to ORB_SLAM package directory.");
        ros::shutdown();
        return 1;
    }
    ORB_SLAM::ORBVocabulary Vocabulary;
    Vocabulary.load(fsVoc);
    ROS_INFO("Vocabulary loaded!");

    //Create the map database
    ORB_SLAM::MapDatabase WorldDB(&Vocabulary);
    FramePub.SetMapDB(&WorldDB);

    //Create Map Publisher for Rviz
    ORB_SLAM::MapPublisher MapPub(&WorldDB);

    //Initialize the Tracking Thread, Local Mapping Thread and Loop Closing Thread
    ORB_SLAM::Tracking Tracker(&FramePub, &MapPub, &WorldDB, &fps_counter, strSettingsFile);
    ORB_SLAM::Relocalization Relocalizer(&WorldDB);
    ORB_SLAM::LocalMapping LocalMapper(&WorldDB);
    ORB_SLAM::LoopClosing LoopCloser(&WorldDB);
    ORB_SLAM::MapMerging MapMerger(&WorldDB);
    
    // Start threads for all
    boost::thread trackingThread(&ORB_SLAM::Tracking::Run,&Tracker);
    boost::thread trackingRelocalizer(&ORB_SLAM::Relocalization::Run, &Relocalizer);
    boost::thread localMappingThread(&ORB_SLAM::LocalMapping::Run,&LocalMapper);
    boost::thread loopClosingThread(&ORB_SLAM::LoopClosing::Run, &LoopCloser);
    boost::thread mapMergingThread(&ORB_SLAM::MapMerging::Run, &MapMerger);
    
    //Set pointers between threads
    Tracker.SetThreads(&LocalMapper, &LoopCloser, &MapMerger, &Relocalizer, &Tracker);
    Relocalizer.SetThreads(&LocalMapper, &LoopCloser, &MapMerger, &Relocalizer, &Tracker);
    LocalMapper.SetThreads(&LocalMapper, &LoopCloser, &MapMerger, &Relocalizer, &Tracker);
    LoopCloser.SetThreads(&LocalMapper, &LoopCloser, &MapMerger, &Relocalizer, &Tracker);
    MapMerger.SetThreads(&LocalMapper, &LoopCloser, &MapMerger, &Relocalizer, &Tracker);

    //This "main" thread will show the current processed frame and publish the map
    float fps = fsSettings["Camera.fps"];
    if(fps==0)
        fps=30;

    ros::Rate r1(fps);
    while (ros::ok())
    {
        // Call each publisher to update
        FramePub.Refresh();
        MapPub.Refresh();
        // If tracking needs to delete a map
        // Check if a stop is requested
        if(Tracker.publishersStopRequested())
        {
            ros::Rate r2(200);
            while(Tracker.publishersStopRequested() && ros::ok())
            {
                Tracker.publishersSetStop(true);
                r2.sleep();
            }
            // Clear out all old data
            FramePub.Reset();
            MapPub.Reset();
        }
        // Show that we are running
        Tracker.publishersSetStop(false);
        // Sleep at our fps
        r1.sleep();
    }
    
    // Nice new line
    cout << endl;
    
    std::string path_str;
    path_str += "mkdir -p ";
    path_str += ros::package::getPath("orb_slam");
    path_str += "/generated/";
    // Create our directory if needed
    const int dir_err = system(path_str.c_str());
    if (-1 == dir_err) {
        cout << "Error creating directory!" << endl;
    }
    
    // Clear old generated folder
    boost::filesystem::path path = ros::package::getPath("orb_slam") + "/generated/";
    for (boost::filesystem::directory_iterator end_dir_it, it(path); it!=end_dir_it; ++it) {
        boost::filesystem::remove_all(it->path());
    }

    // Save keyframe poses at the end of the execution
    for (std::size_t i = 0; i < WorldDB.getAll().size(); ++i) {
        // Check if erased
        if(WorldDB.getAll().at(i)->getErased())
            continue;
        // Output stream
        ofstream f;
        // Get keyframes of current map
        vector<ORB_SLAM::KeyFrame*> vpKFs = WorldDB.getAll().at(i)->GetAllKeyFrames();
        sort(vpKFs.begin(),vpKFs.end(),ORB_SLAM::KeyFrame::lId);
        // Export information, open file
        cout << "Saving Data:   /generated/KeyFrameTrajectory_" << i << ".txt"<< endl;
        std::ostringstream oss;
        oss << ros::package::getPath("orb_slam") << "/generated/KeyFrameTrajectory_" << i << ".txt";
        f.open(oss.str().c_str());
        f << fixed;
        // Export frames
        for(size_t i=0; i<vpKFs.size(); i++)
        {
            ORB_SLAM::KeyFrame* pKF = vpKFs[i];

            if(pKF->isBad())
                continue;

            cv::Mat R = pKF->GetRotation().t();
            vector<float> q = ORB_SLAM::Converter::toQuaternion(R);
            cv::Mat t = pKF->GetCameraCenter();
            // Timestamp: t
            // Position: x, y, z
            // Quaternions: q0, q1, q2, q3
//            float roll =  atan2(2*(q[0]*q[1] + q[3]*q[2]), q[3]*q[3] + q[0]*q[0] - q[1]*q[1] - q[2]*q[2]);
//            float pitch =  atan2(2*(q[1]*q[1] + q[3]*q[0]), q[3]*q[3] - q[0]*q[0] - q[1]*q[1] + q[2]*q[2]);
//            float yaw =  asin(-2*(q[0]*q[2] - q[3]*q[1]));
            
            f << setprecision(6) << pKF->mTimeStamp << setprecision(7) 
                << " " << t.at<float>(0) << " " << t.at<float>(1) << " " << t.at<float>(2)
                << " " << q[0] << " " << q[1] << " " << q[2] << " " << q[3] << endl;

        }
        // Close file
        f.close();
    }
    ros::shutdown();

	return 0;
}
