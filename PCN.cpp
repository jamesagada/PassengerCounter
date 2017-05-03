#ifndef PCN_CPP
#define PCN_CPP

#include "PCN.h"

PCN::PCN(int device)
{
    cap.open(device);

    int history = 1000;
    double varThreshold = BACKGROUN_SUB_THRESHOLD;
    bool detectShadows = true;

#ifdef ReliaGate
    pMOG2 = createBackgroundSubtractorMOG2(history, varThreshold, detectShadows);
#else
    pMOG2 = new BackgroundSubtractorMOG2(history, varThreshold, detectShadows);
#endif
}

void PCN::toggleCalibration()
{
    calibrationOn = !calibrationOn;
    destroyAllWindows();

    return;
}

void PCN::toggleDisplayColor()
{
    displayColor = !displayColor;
    destroyAllWindows();

    return;
}

void PCN::toggleDisplayBacksub()
{
    displayBacksub = !displayBacksub;
    destroyAllWindows();

    return;
}

void PCN::toggleDisplayDenoise()
{
    displayDenoise = !displayDenoise;
    destroyAllWindows();

    return;
}

void PCN::start()
{
    thread_ = std::thread(&PCN::count, this);
    
    auto myid = thread_.get_id();
    stringstream ss;
    ss << myid;
    threadID = ss.str();
}

void PCN::count()
{
    // This is needed to avoid threading problems with GTK
    XInitThreads();

    // Streams
    Mat color;
    Mat fgMaskMOG2;  //fg mask generated by MOG2 method
    Mat morphTrans;
    Mat denoisedMat;

    // Videos
    VideoWriter outputVideoColor;
    // VideoWriter outputVideoBacksub;
    // VideoWriter outputVideoDenoise;

    // Contours variables
    vector<vector<Point> > contours;
    vector<Vec4i> hierarchy;

    // Calibration
    int learningRate = LEARNINGRATE;
    int whiteThreshold = THRESHOLD;
    int dilateAmount = DILATE_AMOUNT;
    int erodeAmount = ERODE_AMOUNT;
    int blur_ksize = BLUR_KSIZE;
    int areaMin = AREA_MIN;
    int xNear = X_NEAR;
    int yNear = Y_NEAR;
    int maxPassengerAge = MAX_PASSENGER_AGE;

    // Execution time
    duration<double> loopTime;
    bool firstLoop = true;

    // --SETUP WINDOWS
    if(displayColor)
        namedWindow("Color threadID: " + threadID,WINDOW_AUTOSIZE);

    if(displayBacksub)
        namedWindow("BackSub threadID: " + threadID,WINDOW_AUTOSIZE);

    if(displayDenoise)
        namedWindow("Denoise threadID: " + threadID,WINDOW_AUTOSIZE);

    if(saveVideo)
    {
        Size S = Size((int) cap.get(CV_CAP_PROP_FRAME_WIDTH), (int) cap.get(CV_CAP_PROP_FRAME_HEIGHT));
        outputVideoColor.open(threadID + "-color.avi", CV_FOURCC('M','J','P','G'), FRAMERATE, S, true);
        // outputVideoBacksub.open(threadID + "-backsub.avi", CV_FOURCC('M','J','P','G'), FRAMERATE, S, true);
        // outputVideoDenoise.open(threadID + "-denoise.avi", CV_FOURCC('M','J','P','G'), FRAMERATE, S, true);
    }

    // --GRAB AND WRITE LOOP
    if(!cap.isOpened()) {
        cerr << "ERROR! Unable to open camera\n";
    }

    // Framerate calculation variables
    int frames = 0; 
    float time = 0, fps = 0;
    auto tf0 = std::chrono::high_resolution_clock::now();

    while(!halt)
    {
        //-- PERFORMANCE ESTMATION
        high_resolution_clock::time_point t1 = high_resolution_clock::now(); //START

        // Wait for a frame from camera/video and store it into frame
        bool bSuccess = cap.read(color);

        // Check for errors
        if (!bSuccess) //if not success, break loop
        {
            cout << "ERROR! Cannot read the frame from video file\n";
            break;
        }

        if (color.empty()) {
            cerr << "ERROR! Blank frame grabbed\n";
            break;
        }

        // Framerate
        auto tf1 = std::chrono::high_resolution_clock::now();
        time += std::chrono::duration<float>(tf1-tf0).count();
        tf0 = tf1;
        ++frames;
        if(time > 0.5f)
        {
            fps = frames / time;
            frames = 0;
            time = 0;
        }
        
	    // --BACKGROUND SUBTRACTION
#ifndef ReliaGate
        pMOG2->operator()(color, fgMaskMOG2, (float)(learningRate/10000.0));
#else
        pMOG2->apply(color, fgMaskMOG2, (float)(learningRate/100));
#endif

        // --MORPHOLOGICAL TRANSFORMATION
        // Threshold the image
        threshold(fgMaskMOG2, morphTrans, whiteThreshold, MAXRGBVAL, THRESH_BINARY);

        // Eroding
        erode(morphTrans,morphTrans, Mat(Size(erodeAmount,erodeAmount), CV_8UC1));

        // Dilating
        dilate(morphTrans,morphTrans, Mat(Size(dilateAmount,dilateAmount), CV_8UC1));

        // Blurring the image
        blur(morphTrans,morphTrans, Size(blur_ksize,blur_ksize));
        denoisedMat = morphTrans.clone();

        // --FINDING CONTOURS
        findContours(morphTrans, contours, hierarchy, RETR_EXTERNAL, CHAIN_APPROX_NONE);

        // For every detected object
        for(unsigned int idx = 0; idx < contours.size(); idx++)
        {
            // Draw contours for every detected object
            // drawContours( color, contours, idx, Scalar(0,255,0), 2, 8, hierarchy, 0, Point(0,0) );

            // -- AREA
            // Calculating area
            double areaCurrentObject = contourArea(contours[idx]);

            // If calculated area is big enough begin tracking the object
            if(areaCurrentObject > areaMin)
            {
                // Getting bounding rectangle
                Rect br = boundingRect(contours[idx]);
                Point2f mc = Point2f((int)(br.x + br.width/2) ,(int)(br.y + br.height/2) );

                // Drawing mass center and bounding rectangle
                rectangle( color, br.tl(), br.br(), GREEN, 2, 8, 0 );
                circle( color, mc, 5, RED, 2, 8, 0 );

                // --PASSENGERS DB UPDATE
                bool newPassenger = true;
                for(unsigned int i = 0; i < passengers.size(); i++)
                {
                    // If the passenger is near a known passenger assume they are the same one
                    if( abs(mc.x - passengers[i].getCurrentPoint().x) <= xNear &&
                        abs(mc.y - passengers[i].getCurrentPoint().y) <= yNear )
                    {
                        // Update coordinates
                        newPassenger = false;
                        passengers[i].updateCoords(mc);

                        // --COUNTER
                        if(passengers[i].getTracks().size() > 1)
                        {
                            // Up to down
                            if( (passengers[i].getLastPoint().y < color.rows/2 && passengers[i].getCurrentPoint().y >= color.rows/2) ||
                                (passengers[i].getLastPoint().y <= color.rows/2 && passengers[i].getCurrentPoint().y > color.rows/2) )
                            {
                                // Counting multiple passenger depending on area size
                                if (areaCurrentObject > MAX_1PASS_AREA && areaCurrentObject < MAX_2PASS_AREA)
                                    cnt_out += 2;
                                else if (areaCurrentObject > MAX_2PASS_AREA)
                                    cnt_out += 3;
                                else
                                    cnt_out++;

                                // Visual feedback
                                circle(color, Point(color.cols - 20, 20), 8, RED, CV_FILLED);
                            }

                            // Down to up
                            if( (passengers[i].getLastPoint().y > color.rows/2 && passengers[i].getCurrentPoint().y <= color.rows/2) ||
                                (passengers[i].getLastPoint().y >= color.rows/2 && passengers[i].getCurrentPoint().y < color.rows/2) )
                            {
                                // Counting multiple passenger depending on area size
                                if (areaCurrentObject > MAX_1PASS_AREA && areaCurrentObject < MAX_2PASS_AREA)
                                    cnt_in += 2;
                                else if (areaCurrentObject > MAX_2PASS_AREA)
                                    cnt_in += 3;
                                else
                                    cnt_in++;

                                // Visual feedback
                                circle(color, Point(color.cols - 20, 20), 8, GREEN, CV_FILLED);
                            }

                        }

                        break;
                    }
                }

                // If wasn't near any known object is a new passenger
                if(newPassenger)
                {
                    Passenger p(pid, mc);
                    passengers.push_back(p);
                    pid++;
                }
            }
        }

        // For every passenger in passengers DB
        for(unsigned int i = 0; i < passengers.size(); i++)
        {
            // -- DRAWING PASSENGER TRAJECTORIES
            if(passengers[i].getTracks().size() > 1)
            {
                polylines(color, passengers[i].getTracks(), false, passengers[i].getTrackColor(),2);
                putText(color, "Pid: " + to_string(passengers[i].getPid()), passengers[i].getCurrentPoint(), FONT_HERSHEY_SIMPLEX, 0.5, passengers[i].getTrackColor(), 2);
            }

            // --UPDATE PASSENGER STATS
            // Updating age
            passengers[i].updateAge();

            // Removing older passengers
            // NB: The age depends on the FPS that the camera is capturing!
            if(passengers[i].getAge() > (maxPassengerAge * fps) )
            {
                passengers.erase(passengers.begin() +i);
            }
        }

        // Debugging
        // putText(color, "Tracked passengers: " + to_string(passengers.size()), Point(15,  15) , FONT_HERSHEY_SIMPLEX, 0.5, RED, 2);
        putText(color, "FPS: " + to_string(fps), Point(0,  15) , FONT_HERSHEY_SIMPLEX, 0.5, RED, 2);

        // --PRINTING INFORMATION
        // Horizontal line     
        line( color,
              Point(0,color.rows/2),            //Starting point of the line
              Point(color.cols,color.rows/2),   //Ending point of the line
              RED,                              //Color
              2,                                //Thickness
              8);                               //Linetype

        putText(color, "Count IN: "  + to_string(cnt_in), Point(0,color.rows - 30) , FONT_HERSHEY_SIMPLEX, 0.5, WHITE, 2);
        putText(color, "Count OUT: " + to_string(cnt_out), Point(0, color.rows - 10) , FONT_HERSHEY_SIMPLEX, 0.5, WHITE, 2);

        // --CALIBRATION TRACKBARS
        if(calibrationOn && displayColor)
        {
            createTrackbar("Learning reate  ", "Color threadID: " + threadID, &learningRate, 1000);
            createTrackbar("White threshold ", "Color threadID: " + threadID, &whiteThreshold, 400);
            createTrackbar("Blur [matrix size]", "Color threadID: " + threadID, &blur_ksize, 100);
            createTrackbar("xNear [pixels]", "Color threadID: " + threadID, &xNear, IMAGEWIDTH);
            createTrackbar("yNear [pixels]", "Color threadID: " + threadID, &yNear, IMAGEHEIGHT);
            createTrackbar("Area min [pixels^2]", "Color threadID: " + threadID, &areaMin, 100000);
            createTrackbar("Passenger age [seconds]", "Color threadID: " + threadID, &maxPassengerAge, 30);
        }

        // Show streams
        if(displayColor)
            imshow("Color threadID: " + threadID, color);

        if(displayBacksub)
            imshow("Backsub threadID: " + threadID, fgMaskMOG2);

        if(displayDenoise)
            imshow("Denoise threadID: " + threadID, denoisedMat);
            
        // -- SAVING VIDEOS
        if(saveVideo)
        {
            outputVideoColor.write(color);
            // outputVideoBacksub.write(fgMaskMOG2);
            // outputVideoDenoise.write(denoisedMat);
        }

        // --PERFORMANCE ESTMATION
        high_resolution_clock::time_point t2 = high_resolution_clock::now(); //STOP

        if(firstLoop)
            loopTime = duration_cast<duration<double>>(t2 - t1);
        else
        {
            loopTime += duration_cast<duration<double>>(t2 - t1);
            loopTime = loopTime/2;
        }

        waitKey(1);

        firstLoop = false;

    }

    cout << "Loop execution time for PCN background subtraction : " << loopTime.count() << " seconds\n";

    // Close windows
    if(displayColor)
        destroyWindow("Color threadID: " + threadID);

    if(displayBacksub)
        destroyWindow("Backsub threadID: " + threadID);

    if(displayDenoise)
        destroyWindow("Denoise threadID: " + threadID);

    // -- SAVING VIDEOS
    if(saveVideo)
    {
        outputVideoColor.release();
        // outputVideoBacksub.release();
        // outputVideoDenoise.release();
    }
        
    // Disable streams
    cap.release();

    return;
}

#endif
