# Runs in Solid Queue's worker processes: a read and two writes on the primary
# database from another process than the web server's, and a cache write
class OrderJob < ApplicationJob
  queue_as :default

  def perform(order_id)
    order = Order.find(order_id)
    Order.transaction do
      order.product.update!(stock: order.product.stock - order.qty)
      order.update!(status: "fulfilled at #{Time.now.utc.iso8601(6)} by #{Process.pid}")
    end
    Rails.cache.write("last_order", order.id)
  end
end
