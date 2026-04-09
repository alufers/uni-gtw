import preact from '@preact/preset-vite'
import tailwindcss from '@tailwindcss/vite'

export default {
  plugins: [tailwindcss(), preact()],
  build: {
    rollupOptions: {
      output: {
        entryFileNames: 'app.js',
        assetFileNames: (info) => info.name.endsWith('.css') ? 'app.css' : info.name,
      }
    }
  }
}
