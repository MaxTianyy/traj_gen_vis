#include "auto_chaser/ObjectHandler.h"

ObjectsHandler::ObjectsHandler(ros::NodeHandle nh){};

void ObjectsHandler::init(ros::NodeHandle nh){

    // parameters
    nh.param<string>("world_frame_id",this->world_frame_id,"/world");
    nh.param<string>("target_frame_id",this->target_frame_id,"/target__base_footprint");
    nh.param<string>("chaser_frame_id",this->chaser_frame_id,"/firefly/base_link"); 

    // edf grid param
    nh.param("min_z",min_z,0.4);            
    nh.param("edf_max_dist",edf_max_dist,2.0);  
    nh.param("edf_max_plot_dist",edf_max_viz_dist,0.5);  
    nh.param("edf_resolution",edf_grid_params.resolution,0.5);  
    nh.param("edf_stride_resolution",edf_grid_params.ray_stride_res,0.5);  

    target_pose.header.frame_id = world_frame_id;
    chaser_pose.header.frame_id = world_frame_id;
    

    // topics 
    tf_listener = new (tf::TransformListener);


    // octomap            
    nh.param("is_octomap_full",this->is_octomap_full,true);
    octree_ptr.reset(new octomap::OcTree(0.1)); // arbitrary init
    if (is_octomap_full)
        sub_octomap = nh.subscribe("/octomap_full",1,&ObjectsHandler::octomap_callback,this);   
    else
        sub_octomap = nh.subscribe("/octomap_binary",1,&ObjectsHandler::octomap_callback,this);   

    ROS_INFO("Object handler initialized."); 
}

void ObjectsHandler::octomap_callback(const octomap_msgs::Octomap& msg){
    // we receive only once from octoamp server
    if (not is_map_recieved){

        // octomap subscribing 
        octomap::AbstractOcTree* octree;

        if(is_octomap_full)
            octree=octomap_msgs::fullMsgToMap(msg);
        else
            octree=octomap_msgs::binaryMsgToMap(msg);

        this->octree_ptr.reset((dynamic_cast<octomap::OcTree*>(octree)));

        ROS_INFO_ONCE("[Objects handler] octomap received.");
        double x,y,z;
        octree_ptr.get()->getMetricMin(x,y,z);
        octomap::point3d boundary_min(x,y,z); 
        octree_ptr.get()->getMetricMax(x,y,z);
        octomap::point3d boundary_max(x,y,z); 

        bool unknownAsOccupied = false;


        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

        // Euclidean distance transform  
        edf_ptr = new DynamicEDTOctomap(edf_max_dist,octree_ptr.get(),
        boundary_min,
        boundary_max,unknownAsOccupied);
        edf_ptr->update();    

        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        double diff = std::chrono::duration_cast<chrono::nanoseconds>( end - begin ).count()*1e-9;
        ROS_INFO("[Objects handler] dynamic EDT computed in %f [sec]",diff);

        // generate homogenous grid 
        edf_grid_params.x0 = boundary_min.x();
        edf_grid_params.y0 = boundary_min.y();
        edf_grid_params.z0 = min_z;
        edf_grid_params.lx = boundary_max.x() - boundary_min.x();
        edf_grid_params.ly = boundary_max.y() - boundary_min.y();
        edf_grid_params.lz = (boundary_max.z() - min_z);
        edf_grid_ptr.reset(new GridField(edf_grid_params));
        compute_edf();

        is_map_recieved = true;
    }
};

// retrive 
PoseStamped ObjectsHandler::get_target_pose() {
    PoseStamped pose(target_pose); 
    pose.pose.position.z = min_z; 
    return pose;
};
 
PoseStamped ObjectsHandler::get_chaser_pose() {return chaser_pose;};

octomap::OcTree* ObjectsHandler::get_octree_obj_ptr() {return octree_ptr.get();};

// callback 
void ObjectsHandler::tf_update(){
    string objects_frame_id[2];
    objects_frame_id[0] = target_frame_id;
    objects_frame_id[1] = chaser_frame_id;
    
    for (int i=0;i<2;i++){            
        tf::StampedTransform transform;    
        // 
        try{
            tf_listener->lookupTransform(world_frame_id,objects_frame_id[i],ros::Time(0), transform);
            PoseStamped pose_stamped;
            pose_stamped.header.stamp = ros::Time::now();
            pose_stamped.header.frame_id = world_frame_id;

            pose_stamped.pose.position.x = transform.getOrigin().getX();
            pose_stamped.pose.position.y = transform.getOrigin().getY();
            pose_stamped.pose.position.z = transform.getOrigin().getZ();

            pose_stamped.pose.orientation.x = transform.getRotation().getX();
            pose_stamped.pose.orientation.y = transform.getRotation().getY();
            pose_stamped.pose.orientation.z = transform.getRotation().getZ();
            pose_stamped.pose.orientation.w = transform.getRotation().getW();



            if (i==0)
                {ROS_INFO_ONCE("tf of target received. "); is_target_recieved = true; target_pose = pose_stamped;} 
            else
                {ROS_INFO_ONCE("tf of chaser received. "); is_chaser_recieved = true; chaser_pose = pose_stamped;}  

        }
        catch (tf::TransformException ex){
            if (i==0)
                ROS_ERROR_ONCE("tf of target does not exist. ",ex.what());  
            else
                ROS_ERROR_ONCE("tf of chaser does not exist. ",ex.what());  
        
        }
    }
}

void ObjectsHandler::compute_edf(){

    for(int ix = 0 ; ix<edf_grid_ptr.get()->Nx ; ix++)
        for(int iy = 0 ; iy<edf_grid_ptr.get()->Ny ; iy++)
            for(int iz = 0 ; iz<edf_grid_ptr.get()->Nz ; iz++){
                Point eval_pnt = edf_grid_ptr.get()->getCellPnt(Vector3i(ix,iy,iz));  
                // query edf value from edf mapper                       
                float dist_val = edf_ptr->getDistance(octomap::point3d(eval_pnt.x,eval_pnt.y,eval_pnt.z));

                // edf value assign to homogenous grid  
                edf_grid_ptr.get()->field_vals[ix][iy][iz] = dist_val;

                // marker generation
                if(dist_val<edf_max_viz_dist){                
                    // color 
                    std_msgs::ColorRGBA color;                    
                    get_color_dist(dist_val,color,edf_max_viz_dist);

                    // marker 
                    markers_edf.points.push_back(eval_pnt);
                    markers_edf.colors.push_back(color);                    
                }
            }    

}