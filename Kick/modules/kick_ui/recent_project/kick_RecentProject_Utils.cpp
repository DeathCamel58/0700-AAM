#include "kick_ui/recent_project/kick_RecentProject_Utils.h"
namespace ui
{
    /////////////////////////////////////////////////////////////////
    ////////   LoadProjectAndAnimation   ////////////////////////////
    /////////////////////////////////////////////////////////////////

    LoadProjectAndAnimation::LoadProjectAndAnimation()
        : juce::Thread( "LoadProject" )
    {
        ae.addListener( this );
    }

    LoadProjectAndAnimation::~LoadProjectAndAnimation()
    {
        ae.removeListener( this );
    }

    void LoadProjectAndAnimation::startLoading( juce::File project )
    {
        ui::tapLog::write( "Tap: start loading " + project.getFullPathName() + " (loader thread running " + juce::String( (int)isThreadRunning() )
                           + ", animation running " + juce::String( (int)isAnimationRunning() ) + ")" );

        // One load at a time: startThread() does nothing while the thread
        // runs, so this project would not load, and run() could read proj
        // while it is written here.
        if( isThreadRunning() )
        {
            ui::tapLog::write( "Tap: a project is still loading; not loading " + project.getFullPathName() );
            return;
        }

        ui::RecentProject::isProjectLoading = true;
        proj                                = project;
        startThread();
        startAnimation();
    }

    void LoadProjectAndAnimation::onProjectLoaded( juce::File file )
    {
        if( isAnimationRunning() )
        {
            juce::Timer::callAfterDelay( 200, [this]() {
                //No need to free the LoadAnimation object, once LoadProjectAndAnimation static object destroyed then it will automatically destroy LoadAnimation object
                stopAnimation();
                kick_ui::signals::sigScreenOpen( kick_ui::Screen::HomeListView );
                ui::RecentProject::isProjectLoading = false;
            } );
        }
    }

    void LoadProjectAndAnimation::run()
    {
        mt::getAudioInterface().loadProject( proj );
    }
}
