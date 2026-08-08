mod net_helper;
mod session;
mod settings;

use session::SessionState;
use settings::{load_settings, save_settings, Settings};
use std::sync::Arc;
use tauri::State;

#[tauri::command]
fn get_settings(state: State<'_, Arc<SessionState>>) -> Settings {
    state.settings.lock().unwrap().clone()
}

#[tauri::command]
fn update_settings(state: State<'_, Arc<SessionState>>, settings: Settings) -> Result<(), String> {
    save_settings(&settings).map_err(|e| e.to_string())?;
    *state.settings.lock().unwrap() = settings;
    Ok(())
}

#[tauri::command]
fn get_session_status(state: State<'_, Arc<SessionState>>) -> session::SessionStatus {
    state.status()
}

#[tauri::command]
fn start_pico(app: tauri::AppHandle, state: State<'_, Arc<SessionState>>) -> Result<(), String> {
    session::start_pico(app, &state).map_err(|e| e.to_string())
}

#[tauri::command]
fn stop_pico(app: tauri::AppHandle, state: State<'_, Arc<SessionState>>) -> Result<(), String> {
    session::stop_pico(&state);
    let _ = app.emit("session-status", state.status());
    Ok(())
}

#[tauri::command]
fn write_console(state: State<'_, Arc<SessionState>>, data: Vec<u8>) -> Result<(), String> {
    session::write_console(&state, &data).map_err(|e| e.to_string())
}

#[tauri::command]
fn start_mame(app: tauri::AppHandle, state: State<'_, Arc<SessionState>>) -> Result<(), String> {
    // Enable NAT via helper when Wi-Fi TAP requested
    {
        let s = state.settings.lock().unwrap().clone();
        if s.wifi_tap {
            let _ = net_helper::enable();
        }
    }
    session::start_mame_stack(app, &state).map_err(|e| e.to_string())
}

#[tauri::command]
fn stop_mame(app: tauri::AppHandle, state: State<'_, Arc<SessionState>>) -> Result<(), String> {
    session::stop_mame(&state);
    let _ = app.emit("session-status", state.status());
    Ok(())
}

#[tauri::command]
fn stop_pico_and_start_mame(
    app: tauri::AppHandle,
    state: State<'_, Arc<SessionState>>,
) -> Result<(), String> {
    session::stop_pico(&state);
    {
        let s = state.settings.lock().unwrap().clone();
        if s.wifi_tap {
            let _ = net_helper::enable();
        }
    }
    session::start_mame_stack(app.clone(), &state).map_err(|e| e.to_string())?;
    let _ = app.emit("session-status", state.status());
    Ok(())
}

#[tauri::command]
fn net_helper_status() -> net_helper::NetHelperStatus {
    net_helper::status()
}

#[tauri::command]
fn net_helper_install(state: State<'_, Arc<SessionState>>) -> Result<net_helper::NetHelperStatus, String> {
    let root = state.settings.lock().unwrap().megaflash_vm_root.clone();
    let st = net_helper::install(&root).map_err(|e| e.to_string())?;
    state.settings.lock().unwrap().network_helper_installed = st.installed;
    let s = state.settings.lock().unwrap().clone();
    let _ = save_settings(&s);
    Ok(st)
}

#[tauri::command]
fn net_helper_enable() -> Result<String, String> {
    net_helper::enable().map_err(|e| e.to_string())
}

#[tauri::command]
fn net_helper_disable() -> Result<String, String> {
    net_helper::disable().map_err(|e| e.to_string())
}

#[tauri::command]
fn xmodem_upload_message(path: String) -> String {
    session::xmodem_upload_hint(&path)
}

#[tauri::command]
fn sync_firmware(state: State<'_, Arc<SessionState>>) -> Result<String, String> {
    let root = state.settings.lock().unwrap().megaflash_vm_root.clone();
    let script = std::path::Path::new(&root).join("scripts/sync-firmware-from-megaflash.sh");
    let out = std::process::Command::new("bash")
        .arg(&script)
        .current_dir(&root)
        .output()
        .map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(String::from_utf8_lossy(&out.stderr).to_string());
    }
    Ok(String::from_utf8_lossy(&out.stdout).to_string())
}

use tauri::Emitter;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let settings = load_settings();
    let state = Arc::new(SessionState::new(settings));

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_process::init())
        .manage(state)
        .invoke_handler(tauri::generate_handler![
            get_settings,
            update_settings,
            get_session_status,
            start_pico,
            stop_pico,
            write_console,
            start_mame,
            stop_mame,
            stop_pico_and_start_mame,
            net_helper_status,
            net_helper_install,
            net_helper_enable,
            net_helper_disable,
            xmodem_upload_message,
            sync_firmware,
        ])
        .run(tauri::generate_context!())
        .expect("error while running MegaFlash Operator");
}
