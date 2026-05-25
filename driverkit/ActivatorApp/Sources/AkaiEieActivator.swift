import Cocoa
import SystemExtensions

final class AkaiEieActivatorApp: NSObject, NSApplicationDelegate {
	private static let driverIdentifier = "com.interslice.akai-eie-pro.driver"
	private enum RequestPurpose {
		case none
		case activate
		case deactivate
		case reloadDeactivate
		case reloadActivate
	}

	private var window: NSWindow!
	private let statusLabel = NSTextField(labelWithString: "Ready")
	private let logView = NSTextView(frame: NSRect(x: 0, y: 0, width: 500, height: 220))
	private var requestPurpose = RequestPurpose.none
	private var actionButtons: [NSButton] = []

	func applicationDidFinishLaunching(_ notification: Notification) {
		NSApp.setActivationPolicy(.regular)
		buildWindow()
		NSApp.activate(ignoringOtherApps: true)

		append("Driver identifier: \(Self.driverIdentifier)")
		append("Embedded dext: \(embeddedDextExists() ? "present" : "missing")")
		append("App location: \(Bundle.main.bundleURL.path)")
		appendActivationLocationHintIfNeeded()
	}

	func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
		true
	}

	@objc private func activateDriver(_ sender: Any?) {
		guard beginRequest(.activate) else {
			return
		}
		append("Submitting activation request")
		appendActivationLocationHintIfNeeded()
		statusLabel.stringValue = "Activating..."
		submitActivationRequest()
	}

	private func submitActivationRequest() {
		let request = OSSystemExtensionRequest.activationRequest(
			forExtensionWithIdentifier: Self.driverIdentifier,
			queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)
	}

	@objc private func deactivateDriver(_ sender: Any?) {
		guard beginRequest(.deactivate) else {
			return
		}
		append("Submitting deactivation request")
		statusLabel.stringValue = "Deactivating..."
		submitDeactivationRequest()
	}

	@objc private func reloadDriver(_ sender: Any?) {
		guard beginRequest(.reloadDeactivate) else {
			return
		}
		append("Reloading driver: deactivating current dext before activating embedded replacement")
		append("Stop audio playback before reloading so Core Audio releases the device cleanly")
		statusLabel.stringValue = "Reload: deactivating..."
		submitDeactivationRequest()
	}

	private func submitDeactivationRequest() {
		let request = OSSystemExtensionRequest.deactivationRequest(
			forExtensionWithIdentifier: Self.driverIdentifier,
			queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)
	}

	private func beginRequest(_ purpose: RequestPurpose) -> Bool {
		guard requestPurpose == .none else {
			append("A driver request is already in progress")
			return false
		}
		requestPurpose = purpose
		actionButtons.forEach { $0.isEnabled = false }
		return true
	}

	private func completeRequest() {
		requestPurpose = .none
		actionButtons.forEach { $0.isEnabled = true }
	}

	private func buildWindow() {
		let contentView = NSView()
		contentView.translatesAutoresizingMaskIntoConstraints = false

		let titleLabel = NSTextField(labelWithString: "AKAI EIE Pro Driver")
		titleLabel.font = .systemFont(ofSize: 18, weight: .semibold)

		statusLabel.font = .systemFont(ofSize: 13, weight: .regular)
		statusLabel.textColor = .secondaryLabelColor

		let activateButton = NSButton(
			title: "Activate Driver",
			target: self,
			action: #selector(activateDriver(_:)))
		activateButton.bezelStyle = .rounded

		let deactivateButton = NSButton(
			title: "Deactivate Driver",
			target: self,
			action: #selector(deactivateDriver(_:)))
		deactivateButton.bezelStyle = .rounded

		let reloadButton = NSButton(
			title: "Reload Driver",
			target: self,
			action: #selector(reloadDriver(_:)))
		reloadButton.bezelStyle = .rounded

		actionButtons = [activateButton, deactivateButton, reloadButton]
		let buttonStack = NSStackView(views: actionButtons)
		buttonStack.orientation = .horizontal
		buttonStack.spacing = 8
		buttonStack.alignment = .leading

		logView.isEditable = false
		logView.isSelectable = true
		logView.isVerticallyResizable = true
		logView.isHorizontallyResizable = false
		logView.autoresizingMask = [.width]
		logView.minSize = NSSize(width: 0, height: 220)
		logView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
		logView.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
		logView.textContainerInset = NSSize(width: 8, height: 8)
		logView.textContainer?.containerSize = NSSize(width: 500, height: CGFloat.greatestFiniteMagnitude)
		logView.textContainer?.widthTracksTextView = true

		let scrollView = NSScrollView(frame: NSRect(x: 0, y: 0, width: 500, height: 220))
		scrollView.hasVerticalScroller = true
		scrollView.borderType = .bezelBorder
		scrollView.documentView = logView

		let stack = NSStackView(views: [titleLabel, statusLabel, buttonStack, scrollView])
		stack.orientation = .vertical
		stack.alignment = .leading
		stack.spacing = 12
		stack.translatesAutoresizingMaskIntoConstraints = false
		contentView.addSubview(stack)

		NSLayoutConstraint.activate([
			stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 20),
			stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -20),
			stack.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 20),
			stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -20),
			scrollView.widthAnchor.constraint(equalTo: stack.widthAnchor),
			scrollView.heightAnchor.constraint(equalToConstant: 220),
		])

		window = NSWindow(
			contentRect: NSRect(x: 0, y: 0, width: 540, height: 360),
			styleMask: [.titled, .closable, .miniaturizable],
			backing: .buffered,
			defer: false)
		window.center()
		window.isReleasedWhenClosed = false
		window.title = "AKAI EIE Pro Driver"
		window.contentView = contentView
		window.makeKeyAndOrderFront(nil)
	}

	private func embeddedDextExists() -> Bool {
		FileManager.default.fileExists(atPath: embeddedDextURL().path)
	}

	private func embeddedDextURL() -> URL {
		Bundle.main.bundleURL
			.appendingPathComponent("Contents")
			.appendingPathComponent("Library")
			.appendingPathComponent("SystemExtensions")
			.appendingPathComponent("\(Self.driverIdentifier).dext")
	}

	private func appendActivationLocationHintIfNeeded() {
		guard !runningFromApplicationsFolder() else {
			return
		}

		append("App is outside /Applications. Activation requires launching it from /Applications or enabling System Extensions developer mode.")
		append("For local iteration with SIP disabled: systemextensionsctl developer on")
	}

	private func runningFromApplicationsFolder() -> Bool {
		let applicationDirectory = URL(fileURLWithPath: "/Applications", isDirectory: true)
			.resolvingSymlinksInPath()
			.standardizedFileURL.path
		let appPath = Bundle.main.bundleURL
			.resolvingSymlinksInPath()
			.standardizedFileURL.path

		return appPath == applicationDirectory || appPath.hasPrefix(applicationDirectory + "/")
	}

	private func append(_ message: String) {
		let line = "[\(Self.timestamp())] \(message)\n"
		logView.textStorage?.append(NSAttributedString(string: line))
		logView.scrollToEndOfDocument(nil)
	}

	private static func timestamp() -> String {
		let formatter = DateFormatter()
		formatter.dateFormat = "HH:mm:ss"
		return formatter.string(from: Date())
	}
}

extension AkaiEieActivatorApp: OSSystemExtensionRequestDelegate {
	func request(
		_ request: OSSystemExtensionRequest,
		actionForReplacingExtension existing: OSSystemExtensionProperties,
		withExtension ext: OSSystemExtensionProperties
	) -> OSSystemExtensionRequest.ReplacementAction {
		if requestPurpose == .activate {
			append("Driver is already installed; use Reload Driver to replace it cleanly")
			statusLabel.stringValue = "Use Reload Driver"
			return .cancel
		}

		append("Replacing \(existing.bundleIdentifier) with \(ext.bundleIdentifier)")
		return .replace
	}

	func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
		append("Waiting for user approval in System Settings")
		statusLabel.stringValue = "Waiting for approval"
	}

	func request(
		_ request: OSSystemExtensionRequest,
		didFinishWithResult result: OSSystemExtensionRequest.Result
	) {
		switch result {
		case .completed:
			switch requestPurpose {
			case .deactivate:
				append("Deactivation completed")
				statusLabel.stringValue = "Deactivated"
				completeRequest()
			case .reloadDeactivate:
				append("Deactivation completed; activating embedded replacement")
				statusLabel.stringValue = "Reload: activating..."
				requestPurpose = .reloadActivate
				DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
					self.submitActivationRequest()
				}
			case .reloadActivate:
				append("Reload completed")
				statusLabel.stringValue = "Activated"
				completeRequest()
			case .activate:
				append("Activation completed")
				statusLabel.stringValue = "Activated"
				completeRequest()
			case .none:
				append("Request completed")
				statusLabel.stringValue = "Finished"
				completeRequest()
			}
		case .willCompleteAfterReboot:
			append("Request will complete after reboot")
			statusLabel.stringValue = "Reboot required"
			completeRequest()
		@unknown default:
			append("Request finished with unknown result")
			statusLabel.stringValue = "Finished"
			completeRequest()
		}
	}

	func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
		let nsError = error as NSError
		append("Request failed: \(nsError.domain) \(nsError.code) \(nsError.localizedDescription)")
		if requestPurpose == .reloadDeactivate &&
			nsError.domain == OSSystemExtensionErrorDomain &&
			nsError.code == 4 {
			append("No active extension to deactivate; activating embedded driver")
			statusLabel.stringValue = "Reload: activating..."
			requestPurpose = .reloadActivate
			submitActivationRequest()
			return
		}
		if nsError.domain == OSSystemExtensionErrorDomain && nsError.code == 13 {
			append("macOS authorization is required for deactivation; approve it, then retry Reload Driver")
		}
		appendActivationLocationHintIfNeeded()
		statusLabel.stringValue = "Request failed - see log"
		completeRequest()
	}
}
