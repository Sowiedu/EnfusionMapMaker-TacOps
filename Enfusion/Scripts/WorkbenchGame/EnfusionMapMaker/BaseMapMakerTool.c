//! Base class for EnfusionMapMaker workbench tools.
//! Provides shared boilerplate: API caching, run/cancel state,
//! ESC-key cancellation, and OnDeActivate cleanup.
class BaseMapMakerTool: WorldEditorTool
{
	//------------------------------------------------------------
	// State
	//------------------------------------------------------------

	private WorldEditorAPI m_Api;
	private bool m_Running;
	private bool m_Cancel;

	//------------------------------------------------------------
	// API access
	//------------------------------------------------------------

	protected WorldEditorAPI GetApi()
	{
		if (!m_Api)
		{
			WorldEditor worldEditor = Workbench.GetModule(WorldEditor);
			m_Api = worldEditor.GetApi();
		}
		return m_Api;
	}

	//------------------------------------------------------------
	// Run / cancel helpers
	//------------------------------------------------------------

	protected bool IsRunning()
	{
		return m_Running;
	}

	protected bool IsCancelled()
	{
		return m_Cancel;
	}

	protected void SetRunning(bool running)
	{
		m_Running = running;
	}

	protected void RequestCancel()
	{
		if (m_Running)
		{
			if (m_Cancel)
			{
				Print("Halt already in progress");
			}
			else
			{
				m_Cancel = true;
				Print("Halting ...");
			}
		}
		else
		{
			Print("Nothing is running");
		}
	}

	protected void BeginOperation()
	{
		m_Running = true;
		m_Cancel = false;
	}

	protected void EndOperation()
	{
		m_Running = false;
	}

	//------------------------------------------------------------
	// Lifecycle
	//------------------------------------------------------------

	override void OnDeActivate()
	{
		m_Cancel = true;
		m_Api = null;
	}

	//------------------------------------------------------------
	// Keyboard input
	//------------------------------------------------------------

	override void OnKeyPressEvent(KeyCode key, bool isAutoRepeat)
	{
		if (key == KeyCode.KC_ESCAPE && !isAutoRepeat && m_Running && !m_Cancel)
		{
			m_Cancel = true;
			Print("Escape pressed — halting ...");
		}
	}
}
