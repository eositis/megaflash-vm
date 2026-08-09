mod net_helper;
mod session;
mod settings;
mod xmodem;

use session::SessionState;
use settings::{load_settings, save_settings, Settings};
use std::sync::Arc;
use tauri::{AppHandle, Emitter, State};

fn state_arc(state: &State<'_, Arc<SessionState>>) -> Arc<SessionState> {
    Arc::clone(state)
}

#[tauri::command]
async fn get_settings(state: State<'_, Arc<SessionState>>) -> Result<Settings, String> {
    let state = state_arc(&state);
    Ok(tauri::async_runtime::spawn_blocking(move || state.get_settings_clone())
        .await
        .map_err(|e| e.to_string())?)
}

#[tauri::command]
async fn update_settings(
    state: State<'_, Arc<SessionState>>,
    settings: Settings,
) -> Result<(), String> {
    let state = state_arc(&state);
    tauri::async_runtime::spawn_blocking(move || {
        save_settings(&settings).map_err(|e| e.to_string())?;
        state.set_settings(settings);
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn get_session_status(
    state: State<'_, Arc<SessionState>>,
) -> Result<session::SessionStatus, String> {
    let state = state_arc(&state);
    Ok(tauri::async_runtime::spawn_blocking(move || state.status())
        .await
        .map_err(|e| e.to_string())?)
}

#[tauri::command]
async fn start_pico(app: AppHandle, state: State<'_, Arc<SessionState>>) -> Result<(), String> {
    let state = state_arc(&state);
    tauri::async_runtime::spawn_blocking(move || session::start_pico(app, &state))
        .await
        .map_err(|e| e.to_string())?
        .map_err(|e| e.to_string())
}

#[tauri::command]
async fn stop_pico(app: AppHandle, state: State<'_, Arc<SessionState>>) -> Result<(), String> {
    let state = state_arc(&state);
    let status = tauri::async_runtime::spawn_blocking(move || {
        session::stop_pico(&state);
        state.status()
    })
    .await
    .map_err(|e| e.to_string())?;
    let _ = app.emit("session-status", status);
    Ok(())
}

#[tauri::command]
async fn write_console(state: State<'_, Arc<SessionState>>, data: Vec<u8>) -> Result<(), String> {
    let state = state_arc(&state);
    tauri::async_runtime::spawn_blocking(move || session::write_console(&state, &data))
        .await
        .map_err(|e| e.to_string())?
        .map_err(|e| e.to_string())
}

#[tauri::command]
async fn start_mame(app: AppHandle, state: State<'_, Arc<SessionState>>) -> Result<(), String> {
    let state = state_arc(&state);
    tauri::async_runtime::spawn_blocking(move || {
        let s = state.get_settings_clone();
        if s.wifi_tap {
            let _ = net_helper::enable();
        }
        session::start_mame_stack(app, &state)
    })
    .await
    .map_err(|e| e.to_string())?
    .map_err(|e| e.to_string())
}

#[tauri::command]
async fn stop_mame(app: AppHandle, state: State<'_, Arc<SessionState>>) -> Result<(), String> {
    let state = state_arc(&state);
    let status = tauri::async_runtime::spawn_blocking(move || {
        session::stop_mame(&state);
        state.status()
    })
    .await
    .map_err(|e| e.to_string())?;
    let _ = app.emit("session-status", status);
    Ok(())
}

#[tauri::command]
async fn stop_pico_and_start_mame(
    app: AppHandle,
    state: State<'_, Arc<SessionState>>,
) -> Result<(), String> {
    let state = state_arc(&state);
    let app2 = app.clone();
    let status = tauri::async_runtime::spawn_blocking(move || {
        session::stop_pico(&state);
        let s = state.get_settings_clone();
        if s.wifi_tap {
            let _ = net_helper::enable();
        }
        session::start_mame_stack(app2, &state)?;
        Ok::<_, anyhow::Error>(state.status())
    })
    .await
    .map_err(|e| e.to_string())?
    .map_err(|e| e.to_string())?;
    let _ = app.emit("session-status", status);
    Ok(())
}

#[tauri::command]
async fn net_helper_status() -> Result<net_helper::NetHelperStatus, String> {
    Ok(tauri::async_runtime::spawn_blocking(net_helper::status)
        .await
        .map_err(|e| e.to_string())?)
}

#[tauri::command]
async fn net_helper_install(
    state: State<'_, Arc<SessionState>>,
) -> Result<net_helper::NetHelperStatus, String> {
    let state = state_arc(&state);
    tauri::async_runtime::spawn_blocking(move || {
        let root = state.get_settings_clone().megaflash_vm_root;
        let st = net_helper::install(&root).map_err(|e| e.to_string())?;
        let mut s = state.get_settings_clone();
        s.network_helper_installed = st.installed;
        state.set_settings(s.clone());
        let _ = save_settings(&s);
        Ok(st)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn net_helper_enable() -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(|| net_helper::enable().map_err(|e| e.to_string()))
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn net_helper_disable() -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(|| net_helper::disable().map_err(|e| e.to_string()))
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn xmodem_upload(
    app: AppHandle,
    state: State<'_, Arc<SessionState>>,
    path: String,
) -> Result<String, String> {
    let state = state_arc(&state);
    tauri::async_runtime::spawn_blocking(move || {
        session::xmodem_upload(app, &state, std::path::Path::new(&path)).map_err(|e| e.to_string())
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn xmodem_upload_message(path: String) -> Result<String, String> {
    Ok(session::xmodem_upload_hint(&path))
}

#[tauri::command]
async fn sync_firmware(state: State<'_, Arc<SessionState>>) -> Result<String, String> {
    let state = state_arc(&state);
    tauri::async_runtime::spawn_blocking(move || {
        let root = state.get_settings_clone().megaflash_vm_root;
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
    })
    .await
    .map_err(|e| e.to_string())?
}

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
            xmodem_upload,
            xmodem_upload_message,
            sync_firmware,
        ])
        .run(tauri::generate_context!())
        .expect("error while running MegaFlash Operator");
}
