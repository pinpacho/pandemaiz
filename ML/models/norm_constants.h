#pragma once
// Auto-generado por seismic_cnn_trainer.ipynb — NO editar a mano
// Aplicar en ESP32 ANTES de pasar el espectrograma al modelo:
//   x_norm = (log10(PSD + 1e-12) - MEAN) / STD

// Z-score global (calculado sobre el training set)
constexpr float SEISMIC_NORM_MEAN  = -10.08725929f;
constexpr float SEISMIC_NORM_STD   = 1.82254982f;

// Ventana Hanning 128 puntos — scipy.signal.get_window("hann", 128)
// PSD = |FFT|^2 / (FS * HANN_POWER)   (one-sided, igual que scipy)
constexpr float SEISMIC_HANN_POWER = 48.00000000f;
constexpr float SEISMIC_FS         = 200.0f;

// Umbral de clasificacion (score > THRESHOLD -> sismo)
constexpr float SEISMIC_THRESHOLD  = 0.50f;
