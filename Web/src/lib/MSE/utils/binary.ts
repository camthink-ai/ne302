/*
 * @Description: Binary data processing utility
 */

// Data type enumeration
export const dataType = {
    int: 'int',
    uint: 'uint',
    short: 'short',
    ushort: 'ushort',
    byte: 'byte',
    ubyte: 'ubyte',
    float: 'float',
    double: 'double'
}

export type DataType = keyof typeof dataType;
// Get specified type data from ArrayBuffer
export function getValueFromArrayBuffer(type: DataType, buffer: ArrayBuffer, offset: number, count = 1) {
    const view = new DataView(buffer, offset)
    
    switch (type) {
        case dataType.int:
            return count === 1 ? view.getInt32(0, true) : Array.from({ length: count }, (_, i) => view.getInt32(i * 4, true))
        case dataType.uint:
            return count === 1 ? view.getUint32(0, true) : Array.from({ length: count }, (_, i) => view.getUint32(i * 4, true))
        case dataType.short:
            return count === 1 ? view.getInt16(0, true) : Array.from({ length: count }, (_, i) => view.getInt16(i * 2, true))
        case dataType.ushort:
            return count === 1 ? view.getUint16(0, true) : Array.from({ length: count }, (_, i) => view.getUint16(i * 2, true))
        case dataType.byte:
            return count === 1 ? view.getInt8(0) : Array.from({ length: count }, (_, i) => view.getInt8(i))
        case dataType.ubyte:
            return count === 1 ? view.getUint8(0) : Array.from({ length: count }, (_, i) => view.getUint8(i))
        case dataType.float:
            return count === 1 ? view.getFloat32(0, true) : Array.from({ length: count }, (_, i) => view.getFloat32(i * 4, true))
        case dataType.double:
            return count === 1 ? view.getFloat64(0, true) : Array.from({ length: count }, (_, i) => view.getFloat64(i * 8, true))
        default:
            throw new Error(`Unsupported data type: ${type}`)
    }
}

// Write data to ArrayBuffer
export function setValueToArrayBuffer(type: DataType, buffer: ArrayBuffer, offset: number, value: number) {
    const view = new DataView(buffer, offset)
    
    switch (type) {
        case dataType.int:
            view.setInt32(0, value, true)
            break
        case dataType.uint:
            view.setUint32(0, value, true)
            break
        case dataType.short:
            view.setInt16(0, value, true)
            break
        case dataType.ushort:
            view.setUint16(0, value, true)
            break
        case dataType.byte:
            view.setInt8(0, value)
            break
        case dataType.ubyte:
            view.setUint8(0, value)
            break
        case dataType.float:
            view.setFloat32(0, value, true)
            break
        case dataType.double:
            view.setFloat64(0, value, true)
            break
        default:
            throw new Error(`Unsupported data type: ${type}`)
    }
} 