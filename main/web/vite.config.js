import preact from '@preact/preset-vite'

export default {
  plugins: [preact()],
  build: {
    rollupOptions: {
      output: {
        entryFileNames: 'app.js',
        assetFileNames: (info) => info.name.endsWith('.css') ? 'app.css' : info.name,
      }
    }
  }
}
