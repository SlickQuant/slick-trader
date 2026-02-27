/**
 * Chart management for P&L visualization
 */
class ChartManager {
    constructor() {
        this.pnlChart = null;
        this.pnlData = {
            labels: [],
            datasets: [{
                label: 'Total P&L',
                data: [],
                borderColor: '#3b82f6',
                backgroundColor: 'rgba(59, 130, 246, 0.1)',
                borderWidth: 2,
                fill: true,
                tension: 0.4,
                pointRadius: 0,
                pointHoverRadius: 6,
            }, {
                label: 'Realized P&L',
                data: [],
                borderColor: '#10b981',
                backgroundColor: 'rgba(16, 185, 129, 0.05)',
                borderWidth: 2,
                fill: false,
                tension: 0.4,
                pointRadius: 0,
                pointHoverRadius: 6,
            }, {
                label: 'Unrealized P&L',
                data: [],
                borderColor: '#f59e0b',
                backgroundColor: 'rgba(245, 158, 11, 0.05)',
                borderWidth: 2,
                fill: false,
                tension: 0.4,
                pointRadius: 0,
                pointHoverRadius: 6,
            }]
        };
        this.maxDataPoints = 100;
    }

    /**
     * Initialize P&L chart
     */
    initPnLChart(canvasId) {
        const ctx = document.getElementById(canvasId);
        if (!ctx) {
            console.error('[Chart] Canvas element not found:', canvasId);
            return;
        }

        this.pnlChart = new Chart(ctx, {
            type: 'line',
            data: this.pnlData,
            options: {
                responsive: true,
                maintainAspectRatio: false,
                interaction: {
                    mode: 'index',
                    intersect: false,
                },
                plugins: {
                    legend: {
                        display: true,
                        position: 'top',
                        labels: {
                            color: '#e4e6eb',
                            font: {
                                size: 12,
                            },
                            usePointStyle: true,
                            padding: 15,
                        }
                    },
                    tooltip: {
                        backgroundColor: 'rgba(19, 24, 36, 0.95)',
                        titleColor: '#e4e6eb',
                        bodyColor: '#e4e6eb',
                        borderColor: '#2a3142',
                        borderWidth: 1,
                        padding: 12,
                        displayColors: true,
                        callbacks: {
                            label: function(context) {
                                let label = context.dataset.label || '';
                                if (label) {
                                    label += ': ';
                                }
                                if (context.parsed.y !== null) {
                                    label += '$' + context.parsed.y.toFixed(2);
                                }
                                return label;
                            }
                        }
                    }
                },
                scales: {
                    x: {
                        display: true,
                        grid: {
                            color: 'rgba(42, 49, 66, 0.5)',
                            drawBorder: false,
                        },
                        ticks: {
                            color: '#8b92a7',
                            maxRotation: 0,
                            autoSkipPadding: 20,
                        }
                    },
                    y: {
                        display: true,
                        position: 'right',
                        grid: {
                            color: 'rgba(42, 49, 66, 0.5)',
                            drawBorder: false,
                        },
                        ticks: {
                            color: '#8b92a7',
                            callback: function(value) {
                                return '$' + value.toFixed(0);
                            }
                        }
                    }
                },
                animation: {
                    duration: 300,
                }
            }
        });

        console.log('[Chart] P&L chart initialized');
    }

    /**
     * Add data point to P&L chart
     */
    addPnLDataPoint(timestamp, totalPnL, realizedPnL, unrealizedPnL) {
        const timeLabel = this.formatTimeLabel(timestamp);

        // Add new data point
        this.pnlData.labels.push(timeLabel);
        this.pnlData.datasets[0].data.push(totalPnL);
        this.pnlData.datasets[1].data.push(realizedPnL);
        this.pnlData.datasets[2].data.push(unrealizedPnL);

        // Limit data points
        if (this.pnlData.labels.length > this.maxDataPoints) {
            this.pnlData.labels.shift();
            this.pnlData.datasets[0].data.shift();
            this.pnlData.datasets[1].data.shift();
            this.pnlData.datasets[2].data.shift();
        }

        // Update chart
        if (this.pnlChart) {
            this.pnlChart.update('none'); // Update without animation for real-time feel
        }
    }

    /**
     * Update P&L chart with bulk data
     */
    updatePnLChart(dataPoints) {
        if (!dataPoints || dataPoints.length === 0) {
            return;
        }

        // Clear existing data
        this.pnlData.labels = [];
        this.pnlData.datasets[0].data = [];
        this.pnlData.datasets[1].data = [];
        this.pnlData.datasets[2].data = [];

        // Add all data points
        dataPoints.forEach(point => {
            const timeLabel = this.formatTimeLabel(point.timestamp);
            this.pnlData.labels.push(timeLabel);
            this.pnlData.datasets[0].data.push(point.total || 0);
            this.pnlData.datasets[1].data.push(point.realized || 0);
            this.pnlData.datasets[2].data.push(point.unrealized || 0);
        });

        // Update chart
        if (this.pnlChart) {
            this.pnlChart.update();
        }
    }

    /**
     * Format timestamp for chart label
     */
    formatTimeLabel(timestamp) {
        if (!timestamp) {
            return new Date().toLocaleTimeString('en-US', {
                hour: '2-digit',
                minute: '2-digit',
            });
        }

        // Handle nanosecond timestamps
        const date = typeof timestamp === 'number' && timestamp > 1e15
            ? new Date(timestamp / 1000000)
            : new Date(timestamp);

        return date.toLocaleTimeString('en-US', {
            hour: '2-digit',
            minute: '2-digit',
        });
    }

    /**
     * Clear all chart data
     */
    clearChart() {
        this.pnlData.labels = [];
        this.pnlData.datasets[0].data = [];
        this.pnlData.datasets[1].data = [];
        this.pnlData.datasets[2].data = [];

        if (this.pnlChart) {
            this.pnlChart.update();
        }
    }

    /**
     * Destroy chart
     */
    destroy() {
        if (this.pnlChart) {
            this.pnlChart.destroy();
            this.pnlChart = null;
        }
    }

    /**
     * Get current chart data summary
     */
    getChartSummary() {
        const datasets = this.pnlData.datasets;
        const dataLength = this.pnlData.labels.length;

        if (dataLength === 0) {
            return {
                total: 0,
                realized: 0,
                unrealized: 0,
                dataPoints: 0
            };
        }

        const latestIndex = dataLength - 1;

        return {
            total: datasets[0].data[latestIndex] || 0,
            realized: datasets[1].data[latestIndex] || 0,
            unrealized: datasets[2].data[latestIndex] || 0,
            dataPoints: dataLength
        };
    }
}

// Create global chart manager instance
const chartManager = new ChartManager();
