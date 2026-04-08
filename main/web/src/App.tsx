import { Console } from './Console'

export function App() {
  return (
    <div style={{ display: 'flex', height: '100%' }}>
      <div style={{
        flex: 1,
        borderRight: '1px solid #333',
        padding: '16px',
        color: '#666',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
      }}>
        <span>Gateway Controls — TBD</span>
      </div>
      <div style={{ flex: 1, overflow: 'hidden' }}>
        <Console />
      </div>
    </div>
  )
}
