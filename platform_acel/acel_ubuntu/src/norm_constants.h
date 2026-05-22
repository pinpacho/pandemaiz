#pragma once
// Auto-generado por seismic_cnn_trainer.ipynb — NO editar a mano
// Aplicar en ESP32 ANTES de pasar el espectrograma al modelo:
//   x_norm = (log10(PSD + 1e-12) - MEAN) / STD

// Z-score global (calculado sobre el training set)
constexpr float SEISMIC_NORM_MEAN  = -7.98922157f;
constexpr float SEISMIC_NORM_STD   = 1.37405312f;

// Ventana Hanning 128 puntos — scipy.signal.get_window("hann", 128)
// PSD = |FFT|^2 / (FS * HANN_POWER)   (one-sided, igual que scipy)
constexpr float SEISMIC_HANN_POWER = 48.00000000f;
constexpr float SEISMIC_FS         = 200.0f;

// Umbral de clasificacion (score > THRESHOLD -> sismo)
// Calibrado sobre validation set: maximiza F1_sismo con recall>=0.90
constexpr float SEISMIC_THRESHOLD  = 0.50f;

// PGA normalization — por eje, calculado sobre train set
// Generado por seismic_cnn_trainer.ipynb; reemplazar tras re-entrenar.
// Aplicar: pga_norm[c] = (pga_g[c] - MEAN[c]) / STD[c]
constexpr float SEISMIC_PGA_MEAN[3] = {0.01375112f, 0.01821116f, 0.04943195f};
constexpr float SEISMIC_PGA_STD[3]  = {0.06148684f,  0.05301067f,  0.15822884f};
