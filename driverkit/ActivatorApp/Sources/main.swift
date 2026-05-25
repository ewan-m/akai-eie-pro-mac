import Cocoa

let app = NSApplication.shared
let delegate = AkaiEieActivatorApp()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
