-- SmartKnob plugin for Lightroom Classic
-- Install: File > Plug-in Manager > Add, pick this SmartKnob.lrplugin folder.
return {
    LrSdkVersion = 10.0,
    LrSdkMinimumVersion = 6.0,   -- LrSocket needs 6.0+
    LrToolkitIdentifier = 'com.suchchris.smartknob',
    LrPluginName = 'SmartKnob',
    LrInitPlugin = 'Init.lua',
    LrForceInitPlugin = true,    -- start with Lightroom, not only when a menu item is used
    LrShutdownPlugin = 'Shutdown.lua',
    VERSION = { major = 0, minor = 1, revision = 0 },
    -- File > Plug-in Extras > SmartKnob status (also starts it if it isn't running)
    LrExportMenuItems = {
        { title = 'SmartKnob status', file = 'Status.lua' },
    },
}
